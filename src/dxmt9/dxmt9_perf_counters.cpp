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

void report() {
  if (!enabledFlag()) {
    return;
  }
  const Counters& c = counters();
  std::fprintf(
      stderr,
      "[dxmt9-perf] submit_draw=%llu submit_clear=%llu submit_stretch=%llu "
      "stretch_copy=%llu stretch_pass=%llu stretch_full=%llu "
      "submit_present=%llu submit_flush=%llu command_buffers=%llu "
      "metal_buffers=%llu metal_buffer_bytes=%llu pipeline_builds=%llu "
      "pipeline_hit_draw=%llu pipeline_hit_fill=%llu pipeline_hit_stretch=%llu "
      "pipeline_miss_draw=%llu pipeline_miss_fill=%llu pipeline_miss_stretch=%llu "
      "pipeline_build_draw=%llu pipeline_build_fill=%llu pipeline_build_stretch=%llu pipeline_build_present=%llu "
      "render_pass_begin=%llu render_pass_end=%llu "
      "render_split_final=%llu render_split_rt_change=%llu render_split_hazard=%llu "
      "render_split_clear=%llu render_split_surface_copy=%llu render_split_stretch=%llu "
      "render_split_readback=%llu render_split_color_fill=%llu render_split_present=%llu render_split_present_acquire=%llu "
      "hazard_probe=%llu hazard_bloom=%llu hazard_exact=%llu hazard_bloom_false_positive=%llu "
      "draw_calls=%llu draw_indexed=%llu draw_expanded_indexed=%llu "
      "draw_primitives=%llu draw_triangles=%llu draw_vertices=%llu "
      "draw_up_vertex_bytes=%llu draw_up_index_bytes=%llu "
      "bind_texture=%llu bind_sampler=%llu bind_vertex_buffer=%llu bind_index_buffer=%llu "
      "bind_uniform_buffer=%llu bind_pipeline=%llu bind_depth_state=%llu bind_viewport=%llu bind_scissor=%llu bind_rasterizer=%llu "
      "draw_shader_bucket_samples=%llu draw_shader_bucket_changes=%llu "
      "last_vs=0x%llx last_ps=0x%llx last_variant=0x%llx "
      "draw_geometry_samples=%llu draw_geometry_ffp=%llu draw_geometry_vs=%llu "
      "draw_geometry_indexed=%llu draw_geometry_index16=%llu draw_geometry_index32=%llu "
      "draw_geometry_direct=%llu draw_geometry_up=%llu draw_geometry_expanded=%llu "
      "draw_geometry_nonzero_base_vertex=%llu draw_geometry_nonzero_start_index=%llu "
      "draw_geometry_nonzero_stream0_offset=%llu draw_geometry_last_stream0_stride=%llu "
      "draw_geometry_last_decl_hash=0x%llx "
      "completion_compat_fp16_ms=%.3f completion_compat_mrt_ms=%.3f completion_compat_srgb_ms=%.3f "
      "completion_compat_projected_ms=%.3f completion_compat_msaa_ms=%.3f completion_compat_query_ms=%.3f "
      "completion_shader_bucket_samples=%llu completion_shader_bucket_changes=%llu "
      "completion_last_vs=0x%llx completion_last_ps=0x%llx completion_last_variant=0x%llx "
      "submit_draw_cpu_ms=%.3f submit_draw_cpu_max_ms=%.3f "
      "submit_draw_cpu_p50_ms=%.3f submit_draw_cpu_p95_ms=%.3f submit_draw_cpu_p99_ms=%.3f "
      "encode_chunk_calls=%llu encode_chunk_cpu_ms=%.3f encode_chunk_cpu_max_ms=%.3f "
      "encode_chunk_cpu_p50_ms=%.3f encode_chunk_cpu_p95_ms=%.3f encode_chunk_cpu_p99_ms=%.3f "
      "encode_draw_cpu_ms=%.3f encode_draw_cpu_max_ms=%.3f "
      "encode_draw_cpu_p50_ms=%.3f encode_draw_cpu_p95_ms=%.3f encode_draw_cpu_p99_ms=%.3f "
      "encode_draw_pipeline_lookup_cpu_ms=%.3f encode_draw_pipeline_lookup_cpu_max_ms=%.3f "
      "encode_draw_pipeline_lookup_cpu_p50_ms=%.3f encode_draw_pipeline_lookup_cpu_p95_ms=%.3f encode_draw_pipeline_lookup_cpu_p99_ms=%.3f "
      "encode_draw_uniform_build_cpu_ms=%.3f encode_draw_uniform_build_cpu_max_ms=%.3f "
      "encode_draw_uniform_build_cpu_p50_ms=%.3f encode_draw_uniform_build_cpu_p95_ms=%.3f encode_draw_uniform_build_cpu_p99_ms=%.3f "
      "encode_draw_fvf_decode_cpu_ms=%.3f encode_draw_fvf_decode_cpu_max_ms=%.3f "
      "encode_draw_fvf_decode_cpu_p50_ms=%.3f encode_draw_fvf_decode_cpu_p95_ms=%.3f encode_draw_fvf_decode_cpu_p99_ms=%.3f "
      "encode_draw_stream_bind_cpu_ms=%.3f encode_draw_stream_bind_cpu_max_ms=%.3f "
      "encode_draw_stream_bind_cpu_p50_ms=%.3f encode_draw_stream_bind_cpu_p95_ms=%.3f encode_draw_stream_bind_cpu_p99_ms=%.3f "
      "encode_draw_issue_cpu_ms=%.3f encode_draw_issue_cpu_max_ms=%.3f "
      "encode_draw_issue_cpu_p50_ms=%.3f encode_draw_issue_cpu_p95_ms=%.3f encode_draw_issue_cpu_p99_ms=%.3f "
      "transient_upload_calls=%llu transient_upload_bytes=%llu "
      "transient_upload_cpu_ms=%.3f transient_upload_cpu_max_ms=%.3f "
      "transient_upload_cpu_p50_ms=%.3f transient_upload_cpu_p95_ms=%.3f transient_upload_cpu_p99_ms=%.3f "
      "uniform_vs_consts_calls=%llu uniform_vs_consts_bytes=%llu "
      "uniform_ps_consts_calls=%llu uniform_ps_consts_bytes=%llu "
      "uniform_ffp_vs_calls=%llu uniform_ffp_vs_bytes=%llu "
      "uniform_ffp_ps_calls=%llu uniform_ffp_ps_bytes=%llu "
      "uniform_volatile_pushes=%llu "
      "render_pass_load_action_load=%llu render_pass_load_action_clear=%llu render_pass_load_action_dontcare=%llu "
      "render_pass_load_action_depth_load=%llu render_pass_load_action_depth_clear=%llu render_pass_load_action_depth_dontcare=%llu "
      "render_pass_load_action_stencil_load=%llu render_pass_load_action_stencil_clear=%llu render_pass_load_action_stencil_dontcare=%llu "
      "render_pass_store_action_store=%llu render_pass_store_action_dontcare=%llu render_pass_store_action_resolve=%llu "
      "render_pass_store_action_depth_store=%llu render_pass_store_action_depth_dontcare=%llu "
      "render_pass_store_action_stencil_store=%llu render_pass_store_action_stencil_dontcare=%llu "
      "render_pass_tile_preservation_bytes=%llu "
      "command_buffer_create_cpu_ms=%.3f command_buffer_create_cpu_max_ms=%.3f "
      "command_buffer_create_cpu_p50_ms=%.3f command_buffer_create_cpu_p95_ms=%.3f command_buffer_create_cpu_p99_ms=%.3f "
      "command_buffer_commit_cpu_ms=%.3f command_buffer_commit_cpu_max_ms=%.3f "
      "command_buffer_commit_cpu_p50_ms=%.3f command_buffer_commit_cpu_p95_ms=%.3f command_buffer_commit_cpu_p99_ms=%.3f "
      "completion_waits=%llu completion_wait_ms=%.3f completion_wait_max_ms=%.3f "
      "completion_wait_p50_ms=%.3f completion_wait_p95_ms=%.3f completion_wait_p99_ms=%.3f "
      "completion_present_waits=%llu completion_present_wait_ms=%.3f completion_present_wait_max_ms=%.3f "
      "completion_present_wait_p50_ms=%.3f completion_present_wait_p95_ms=%.3f completion_present_wait_p99_ms=%.3f "
      "completion_draw_waits=%llu completion_draw_wait_ms=%.3f completion_draw_wait_max_ms=%.3f "
      "completion_draw_wait_p50_ms=%.3f completion_draw_wait_p95_ms=%.3f completion_draw_wait_p99_ms=%.3f "
      "completion_blit_waits=%llu completion_blit_wait_ms=%.3f completion_blit_wait_max_ms=%.3f "
      "completion_blit_wait_p50_ms=%.3f completion_blit_wait_p95_ms=%.3f completion_blit_wait_p99_ms=%.3f "
      "completion_present_only_waits=%llu completion_present_only_wait_ms=%.3f completion_present_only_wait_max_ms=%.3f "
      "completion_present_only_wait_p50_ms=%.3f completion_present_only_wait_p95_ms=%.3f completion_present_only_wait_p99_ms=%.3f "
      "completion_draw_present_waits=%llu completion_draw_present_wait_ms=%.3f completion_draw_present_wait_max_ms=%.3f "
      "completion_draw_present_wait_p50_ms=%.3f completion_draw_present_wait_p95_ms=%.3f completion_draw_present_wait_p99_ms=%.3f "
      "completion_draw_stretch_waits=%llu completion_draw_stretch_wait_ms=%.3f completion_draw_stretch_wait_max_ms=%.3f "
      "completion_draw_stretch_wait_p50_ms=%.3f completion_draw_stretch_wait_p95_ms=%.3f completion_draw_stretch_wait_p99_ms=%.3f "
      "completion_stretch_waits=%llu completion_stretch_wait_ms=%.3f completion_stretch_wait_max_ms=%.3f "
      "completion_stretch_wait_p50_ms=%.3f completion_stretch_wait_p95_ms=%.3f completion_stretch_wait_p99_ms=%.3f "
      "completion_blit_only_waits=%llu completion_blit_only_wait_ms=%.3f completion_blit_only_wait_max_ms=%.3f "
      "completion_blit_only_wait_p50_ms=%.3f completion_blit_only_wait_p95_ms=%.3f completion_blit_only_wait_p99_ms=%.3f "
      "completion_other_waits=%llu completion_other_wait_ms=%.3f completion_other_wait_max_ms=%.3f "
      "completion_other_wait_p50_ms=%.3f completion_other_wait_p95_ms=%.3f completion_other_wait_p99_ms=%.3f "
      "sync_waits=%llu sync_wait_ms=%.3f sync_wait_max_ms=%.3f "
      "sync_wait_p50_ms=%.3f sync_wait_p95_ms=%.3f sync_wait_p99_ms=%.3f "
      "queue_writer_waits=%llu queue_writer_wait_ms=%.3f queue_writer_wait_max_ms=%.3f "
      "queue_writer_wait_p50_ms=%.3f queue_writer_wait_p95_ms=%.3f queue_writer_wait_p99_ms=%.3f "
      "queue_commit_waits=%llu queue_commit_wait_ms=%.3f queue_commit_wait_max_ms=%.3f "
      "queue_commit_wait_p50_ms=%.3f queue_commit_wait_p95_ms=%.3f queue_commit_wait_p99_ms=%.3f "
      "queue_sequence_waits=%llu queue_sequence_wait_ms=%.3f queue_sequence_wait_max_ms=%.3f "
      "queue_sequence_wait_p50_ms=%.3f queue_sequence_wait_p95_ms=%.3f queue_sequence_wait_p99_ms=%.3f "
      "present_boundary_applied=%llu present_boundary_skipped=%llu "
      "present_boundary_waits=%llu present_boundary_wait_ms=%.3f present_boundary_wait_max_ms=%.3f "
      "present_boundary_wait_p50_ms=%.3f present_boundary_wait_p95_ms=%.3f present_boundary_wait_p99_ms=%.3f "
      "present_encoded=%llu present_skipped=%llu present_full=%llu "
      "present_source_selections=%llu present_source_explicit=%llu present_source_current_backbuffer=%llu "
      "present_source_checks=%llu present_source_valid=%llu present_source_missing_surface=%llu "
      "present_source_missing_texture=%llu present_source_resolve=%llu present_source_invalid_size=%llu "
      "present_source_handle=0x%llx present_source_texture=0x%llx "
      "present_source_size=%llux%llu present_source_fmt=%llu present_source_samples=%llu "
      "present_pass=%llu present_src=%llux%llu present_dst=%llux%llu "
      "present_dst_max=%llux%llu present_acquire_waits=%llu "
      "present_acquire_wait_ms=%.3f present_acquire_wait_max_ms=%.3f "
      "present_acquire_wait_p50_ms=%.3f present_acquire_wait_p95_ms=%.3f present_acquire_wait_p99_ms=%.3f "
      "present_acquire_slow_waits=%llu "
      "present_async_acquire_requests=%llu present_async_acquire_issued=%llu "
      "present_async_acquire_fallbacks=%llu "
      "present_async_acquire_waits=%llu present_async_acquire_wait_ms=%.3f "
      "present_async_acquire_wait_max_ms=%.3f "
      "present_async_acquire_wait_p50_ms=%.3f present_async_acquire_wait_p95_ms=%.3f present_async_acquire_wait_p99_ms=%.3f "
      "present_async_acquire_slow_waits=%llu "
      "present_token_waits=%llu present_token_wait_ms=%.3f "
      "present_token_wait_max_ms=%.3f "
      "present_token_wait_p50_ms=%.3f present_token_wait_p95_ms=%.3f present_token_wait_p99_ms=%.3f "
      "present_token_slow_waits=%llu "
      "present_preacquire_requests=%llu present_preacquire_hits=%llu "
      "present_preacquire_misses=%llu present_preacquire_wait_ms=%.3f "
      "present_preacquire_wait_max_ms=%.3f "
      "present_preacquire_wait_p50_ms=%.3f present_preacquire_wait_p95_ms=%.3f present_preacquire_wait_p99_ms=%.3f "
      "present_set_props_waits=%llu present_set_props_wait_ms=%.3f "
      "present_set_props_wait_p50_ms=%.3f present_set_props_wait_p95_ms=%.3f present_set_props_wait_p99_ms=%.3f\n",
      static_cast<unsigned long long>(load(c.submitDraw)),
      static_cast<unsigned long long>(load(c.submitClear)),
      static_cast<unsigned long long>(load(c.submitStretch)),
      static_cast<unsigned long long>(load(c.stretchBlitCopy)),
      static_cast<unsigned long long>(load(c.stretchRenderPass)),
      static_cast<unsigned long long>(load(c.stretchFullscreen)),
      static_cast<unsigned long long>(load(c.submitPresent)),
      static_cast<unsigned long long>(load(c.submitFlush)),
      static_cast<unsigned long long>(load(c.commandBuffers)),
      static_cast<unsigned long long>(load(c.metalBuffers)),
      static_cast<unsigned long long>(load(c.metalBufferBytes)),
      static_cast<unsigned long long>(load(c.pipelineBuilds)),
      static_cast<unsigned long long>(load(c.pipelineHitDraw)),
      static_cast<unsigned long long>(load(c.pipelineHitFill)),
      static_cast<unsigned long long>(load(c.pipelineHitStretch)),
      static_cast<unsigned long long>(load(c.pipelineMissDraw)),
      static_cast<unsigned long long>(load(c.pipelineMissFill)),
      static_cast<unsigned long long>(load(c.pipelineMissStretch)),
      static_cast<unsigned long long>(load(c.pipelineBuildDraw)),
      static_cast<unsigned long long>(load(c.pipelineBuildFill)),
      static_cast<unsigned long long>(load(c.pipelineBuildStretch)),
      static_cast<unsigned long long>(load(c.pipelineBuildPresent)),
      static_cast<unsigned long long>(load(c.renderPassBegin)),
      static_cast<unsigned long long>(load(c.renderPassEnd)),
      static_cast<unsigned long long>(load(c.renderSplitFinal)),
      static_cast<unsigned long long>(load(c.renderSplitRenderTargetChange)),
      static_cast<unsigned long long>(load(c.renderSplitHazard)),
      static_cast<unsigned long long>(load(c.renderSplitClearBarrier)),
      static_cast<unsigned long long>(load(c.renderSplitSurfaceCopy)),
      static_cast<unsigned long long>(load(c.renderSplitStretchRect)),
      static_cast<unsigned long long>(load(c.renderSplitReadback)),
      static_cast<unsigned long long>(load(c.renderSplitColorFill)),
      static_cast<unsigned long long>(load(c.renderSplitPresent)),
      static_cast<unsigned long long>(load(c.renderSplitPresentAcquire)),
      static_cast<unsigned long long>(load(c.hazardProbeComparisons)),
      static_cast<unsigned long long>(load(c.hazardBloomOverlaps)),
      static_cast<unsigned long long>(load(c.hazardExactOverlaps)),
      static_cast<unsigned long long>(load(c.hazardBloomFalsePositive)),
      static_cast<unsigned long long>(load(c.drawCalls)),
      static_cast<unsigned long long>(load(c.drawIndexedCalls)),
      static_cast<unsigned long long>(load(c.drawExpandedIndexedCalls)),
      static_cast<unsigned long long>(load(c.drawPrimitiveCount)),
      static_cast<unsigned long long>(load(c.drawTriangleEstimate)),
      static_cast<unsigned long long>(load(c.drawVertexCount)),
      static_cast<unsigned long long>(load(c.drawUpVertexBytes)),
      static_cast<unsigned long long>(load(c.drawUpIndexBytes)),
      static_cast<unsigned long long>(load(c.bindTexture)),
      static_cast<unsigned long long>(load(c.bindSampler)),
      static_cast<unsigned long long>(load(c.bindVertexBuffer)),
      static_cast<unsigned long long>(load(c.bindIndexBuffer)),
      static_cast<unsigned long long>(load(c.bindUniformBuffer)),
      static_cast<unsigned long long>(load(c.bindPipeline)),
      static_cast<unsigned long long>(load(c.bindDepthState)),
      static_cast<unsigned long long>(load(c.bindViewport)),
      static_cast<unsigned long long>(load(c.bindScissor)),
      static_cast<unsigned long long>(load(c.bindRasterizer)),
      static_cast<unsigned long long>(load(c.drawShaderBucketSamples)),
      static_cast<unsigned long long>(load(c.drawShaderBucketChanges)),
      static_cast<unsigned long long>(load(c.lastVertexShaderHash)),
      static_cast<unsigned long long>(load(c.lastPixelShaderHash)),
      static_cast<unsigned long long>(load(c.lastShaderVariantHash)),
      static_cast<unsigned long long>(load(c.drawGeometrySamples)),
      static_cast<unsigned long long>(load(c.drawGeometryFfp)),
      static_cast<unsigned long long>(load(c.drawGeometryVs)),
      static_cast<unsigned long long>(load(c.drawGeometryIndexed)),
      static_cast<unsigned long long>(load(c.drawGeometryIndex16)),
      static_cast<unsigned long long>(load(c.drawGeometryIndex32)),
      static_cast<unsigned long long>(load(c.drawGeometryDirect)),
      static_cast<unsigned long long>(load(c.drawGeometryUp)),
      static_cast<unsigned long long>(load(c.drawGeometryExpanded)),
      static_cast<unsigned long long>(load(c.drawGeometryNonZeroBaseVertex)),
      static_cast<unsigned long long>(load(c.drawGeometryNonZeroStartIndex)),
      static_cast<unsigned long long>(load(c.drawGeometryNonZeroStream0Offset)),
      static_cast<unsigned long long>(load(c.drawGeometryLastStream0Stride)),
      static_cast<unsigned long long>(load(c.drawGeometryLastVertexDeclHash)),
      static_cast<double>(load(c.completionCompatFp16WaitNs)) / 1000000.0,
      static_cast<double>(load(c.completionCompatMrtWaitNs)) / 1000000.0,
      static_cast<double>(load(c.completionCompatSrgbWaitNs)) / 1000000.0,
      static_cast<double>(load(c.completionCompatProjectedWaitNs)) / 1000000.0,
      static_cast<double>(load(c.completionCompatMsaaWaitNs)) / 1000000.0,
      static_cast<double>(load(c.completionCompatQueryWaitNs)) / 1000000.0,
      static_cast<unsigned long long>(load(c.completionShaderBucketSamples)),
      static_cast<unsigned long long>(load(c.completionShaderBucketChanges)),
      static_cast<unsigned long long>(load(c.completionLastVertexShaderHash)),
      static_cast<unsigned long long>(load(c.completionLastPixelShaderHash)),
      static_cast<unsigned long long>(load(c.completionLastShaderVariantHash)),
      static_cast<double>(load(c.submitDrawCpuNs)) / 1000000.0,
      static_cast<double>(load(c.submitDrawCpuMaxNs)) / 1000000.0,
      static_cast<double>(c.submitDrawCpuRing.percentile(0.50)) / 1000000.0,
      static_cast<double>(c.submitDrawCpuRing.percentile(0.95)) / 1000000.0,
      static_cast<double>(c.submitDrawCpuRing.percentile(0.99)) / 1000000.0,
      static_cast<unsigned long long>(load(c.encodeChunkCalls)),
      static_cast<double>(load(c.encodeChunkCpuNs)) / 1000000.0,
      static_cast<double>(load(c.encodeChunkCpuMaxNs)) / 1000000.0,
      static_cast<double>(c.encodeChunkCpuRing.percentile(0.50)) / 1000000.0,
      static_cast<double>(c.encodeChunkCpuRing.percentile(0.95)) / 1000000.0,
      static_cast<double>(c.encodeChunkCpuRing.percentile(0.99)) / 1000000.0,
      static_cast<double>(load(c.encodeDrawCpuNs)) / 1000000.0,
      static_cast<double>(load(c.encodeDrawCpuMaxNs)) / 1000000.0,
      static_cast<double>(c.encodeDrawCpuRing.percentile(0.50)) / 1000000.0,
      static_cast<double>(c.encodeDrawCpuRing.percentile(0.95)) / 1000000.0,
      static_cast<double>(c.encodeDrawCpuRing.percentile(0.99)) / 1000000.0,
      static_cast<double>(load(c.encodeDrawPipelineLookupCpuNs)) / 1000000.0,
      static_cast<double>(load(c.encodeDrawPipelineLookupCpuMaxNs)) / 1000000.0,
      static_cast<double>(c.encodeDrawPipelineLookupCpuRing.percentile(0.50)) / 1000000.0,
      static_cast<double>(c.encodeDrawPipelineLookupCpuRing.percentile(0.95)) / 1000000.0,
      static_cast<double>(c.encodeDrawPipelineLookupCpuRing.percentile(0.99)) / 1000000.0,
      static_cast<double>(load(c.encodeDrawUniformBuildCpuNs)) / 1000000.0,
      static_cast<double>(load(c.encodeDrawUniformBuildCpuMaxNs)) / 1000000.0,
      static_cast<double>(c.encodeDrawUniformBuildCpuRing.percentile(0.50)) / 1000000.0,
      static_cast<double>(c.encodeDrawUniformBuildCpuRing.percentile(0.95)) / 1000000.0,
      static_cast<double>(c.encodeDrawUniformBuildCpuRing.percentile(0.99)) / 1000000.0,
      static_cast<double>(load(c.encodeDrawFvfDecodeCpuNs)) / 1000000.0,
      static_cast<double>(load(c.encodeDrawFvfDecodeCpuMaxNs)) / 1000000.0,
      static_cast<double>(c.encodeDrawFvfDecodeCpuRing.percentile(0.50)) / 1000000.0,
      static_cast<double>(c.encodeDrawFvfDecodeCpuRing.percentile(0.95)) / 1000000.0,
      static_cast<double>(c.encodeDrawFvfDecodeCpuRing.percentile(0.99)) / 1000000.0,
      static_cast<double>(load(c.encodeDrawStreamBindCpuNs)) / 1000000.0,
      static_cast<double>(load(c.encodeDrawStreamBindCpuMaxNs)) / 1000000.0,
      static_cast<double>(c.encodeDrawStreamBindCpuRing.percentile(0.50)) / 1000000.0,
      static_cast<double>(c.encodeDrawStreamBindCpuRing.percentile(0.95)) / 1000000.0,
      static_cast<double>(c.encodeDrawStreamBindCpuRing.percentile(0.99)) / 1000000.0,
      static_cast<double>(load(c.encodeDrawIssueCpuNs)) / 1000000.0,
      static_cast<double>(load(c.encodeDrawIssueCpuMaxNs)) / 1000000.0,
      static_cast<double>(c.encodeDrawIssueCpuRing.percentile(0.50)) / 1000000.0,
      static_cast<double>(c.encodeDrawIssueCpuRing.percentile(0.95)) / 1000000.0,
      static_cast<double>(c.encodeDrawIssueCpuRing.percentile(0.99)) / 1000000.0,
      static_cast<unsigned long long>(load(c.transientUploadCalls)),
      static_cast<unsigned long long>(load(c.transientUploadBytes)),
      static_cast<double>(load(c.transientUploadCpuNs)) / 1000000.0,
      static_cast<double>(load(c.transientUploadCpuMaxNs)) / 1000000.0,
      static_cast<double>(c.transientUploadCpuRing.percentile(0.50)) / 1000000.0,
      static_cast<double>(c.transientUploadCpuRing.percentile(0.95)) / 1000000.0,
      static_cast<double>(c.transientUploadCpuRing.percentile(0.99)) / 1000000.0,
      static_cast<unsigned long long>(load(c.uniformVsConstsCalls)),
      static_cast<unsigned long long>(load(c.uniformVsConstsBytes)),
      static_cast<unsigned long long>(load(c.uniformPsConstsCalls)),
      static_cast<unsigned long long>(load(c.uniformPsConstsBytes)),
      static_cast<unsigned long long>(load(c.uniformFfpVsCalls)),
      static_cast<unsigned long long>(load(c.uniformFfpVsBytes)),
      static_cast<unsigned long long>(load(c.uniformFfpPsCalls)),
      static_cast<unsigned long long>(load(c.uniformFfpPsBytes)),
      static_cast<unsigned long long>(load(c.uniformVolatilePushes)),
      static_cast<unsigned long long>(load(c.renderPassLoadActionLoad)),
      static_cast<unsigned long long>(load(c.renderPassLoadActionClear)),
      static_cast<unsigned long long>(load(c.renderPassLoadActionDontCare)),
      static_cast<unsigned long long>(load(c.renderPassLoadActionDepthLoad)),
      static_cast<unsigned long long>(load(c.renderPassLoadActionDepthClear)),
      static_cast<unsigned long long>(load(c.renderPassLoadActionDepthDontCare)),
      static_cast<unsigned long long>(load(c.renderPassLoadActionStencilLoad)),
      static_cast<unsigned long long>(load(c.renderPassLoadActionStencilClear)),
      static_cast<unsigned long long>(load(c.renderPassLoadActionStencilDontCare)),
      static_cast<unsigned long long>(load(c.renderPassStoreActionStore)),
      static_cast<unsigned long long>(load(c.renderPassStoreActionDontCare)),
      static_cast<unsigned long long>(load(c.renderPassStoreActionResolve)),
      static_cast<unsigned long long>(load(c.renderPassStoreActionDepthStore)),
      static_cast<unsigned long long>(load(c.renderPassStoreActionDepthDontCare)),
      static_cast<unsigned long long>(load(c.renderPassStoreActionStencilStore)),
      static_cast<unsigned long long>(load(c.renderPassStoreActionStencilDontCare)),
      static_cast<unsigned long long>(load(c.renderPassTilePreservationBytes)),
      static_cast<double>(load(c.commandBufferCreateCpuNs)) / 1000000.0,
      static_cast<double>(load(c.commandBufferCreateCpuMaxNs)) / 1000000.0,
      static_cast<double>(c.commandBufferCreateCpuRing.percentile(0.50)) / 1000000.0,
      static_cast<double>(c.commandBufferCreateCpuRing.percentile(0.95)) / 1000000.0,
      static_cast<double>(c.commandBufferCreateCpuRing.percentile(0.99)) / 1000000.0,
      static_cast<double>(load(c.commandBufferCommitCpuNs)) / 1000000.0,
      static_cast<double>(load(c.commandBufferCommitCpuMaxNs)) / 1000000.0,
      static_cast<double>(c.commandBufferCommitCpuRing.percentile(0.50)) / 1000000.0,
      static_cast<double>(c.commandBufferCommitCpuRing.percentile(0.95)) / 1000000.0,
      static_cast<double>(c.commandBufferCommitCpuRing.percentile(0.99)) / 1000000.0,
      static_cast<unsigned long long>(load(c.completionWaits)),
      static_cast<double>(load(c.completionWaitNs)) / 1000000.0,
      static_cast<double>(load(c.completionWaitMaxNs)) / 1000000.0,
      static_cast<double>(c.completionWaitRing.percentile(0.50)) / 1000000.0,
      static_cast<double>(c.completionWaitRing.percentile(0.95)) / 1000000.0,
      static_cast<double>(c.completionWaitRing.percentile(0.99)) / 1000000.0,
      static_cast<unsigned long long>(load(c.completionPresentWaits)),
      static_cast<double>(load(c.completionPresentWaitNs)) / 1000000.0,
      static_cast<double>(load(c.completionPresentWaitMaxNs)) / 1000000.0,
      static_cast<double>(c.completionPresentWaitRing.percentile(0.50)) / 1000000.0,
      static_cast<double>(c.completionPresentWaitRing.percentile(0.95)) / 1000000.0,
      static_cast<double>(c.completionPresentWaitRing.percentile(0.99)) / 1000000.0,
      static_cast<unsigned long long>(load(c.completionDrawWaits)),
      static_cast<double>(load(c.completionDrawWaitNs)) / 1000000.0,
      static_cast<double>(load(c.completionDrawWaitMaxNs)) / 1000000.0,
      static_cast<double>(c.completionDrawWaitRing.percentile(0.50)) / 1000000.0,
      static_cast<double>(c.completionDrawWaitRing.percentile(0.95)) / 1000000.0,
      static_cast<double>(c.completionDrawWaitRing.percentile(0.99)) / 1000000.0,
      static_cast<unsigned long long>(load(c.completionBlitWaits)),
      static_cast<double>(load(c.completionBlitWaitNs)) / 1000000.0,
      static_cast<double>(load(c.completionBlitWaitMaxNs)) / 1000000.0,
      static_cast<double>(c.completionBlitWaitRing.percentile(0.50)) / 1000000.0,
      static_cast<double>(c.completionBlitWaitRing.percentile(0.95)) / 1000000.0,
      static_cast<double>(c.completionBlitWaitRing.percentile(0.99)) / 1000000.0,
      static_cast<unsigned long long>(load(c.completionPresentOnlyWaits)),
      static_cast<double>(load(c.completionPresentOnlyWaitNs)) / 1000000.0,
      static_cast<double>(load(c.completionPresentOnlyWaitMaxNs)) / 1000000.0,
      static_cast<double>(c.completionPresentOnlyWaitRing.percentile(0.50)) / 1000000.0,
      static_cast<double>(c.completionPresentOnlyWaitRing.percentile(0.95)) / 1000000.0,
      static_cast<double>(c.completionPresentOnlyWaitRing.percentile(0.99)) / 1000000.0,
      static_cast<unsigned long long>(load(c.completionDrawPresentWaits)),
      static_cast<double>(load(c.completionDrawPresentWaitNs)) / 1000000.0,
      static_cast<double>(load(c.completionDrawPresentWaitMaxNs)) / 1000000.0,
      static_cast<double>(c.completionDrawPresentWaitRing.percentile(0.50)) / 1000000.0,
      static_cast<double>(c.completionDrawPresentWaitRing.percentile(0.95)) / 1000000.0,
      static_cast<double>(c.completionDrawPresentWaitRing.percentile(0.99)) / 1000000.0,
      static_cast<unsigned long long>(load(c.completionDrawStretchWaits)),
      static_cast<double>(load(c.completionDrawStretchWaitNs)) / 1000000.0,
      static_cast<double>(load(c.completionDrawStretchWaitMaxNs)) / 1000000.0,
      static_cast<double>(c.completionDrawStretchWaitRing.percentile(0.50)) / 1000000.0,
      static_cast<double>(c.completionDrawStretchWaitRing.percentile(0.95)) / 1000000.0,
      static_cast<double>(c.completionDrawStretchWaitRing.percentile(0.99)) / 1000000.0,
      static_cast<unsigned long long>(load(c.completionStretchWaits)),
      static_cast<double>(load(c.completionStretchWaitNs)) / 1000000.0,
      static_cast<double>(load(c.completionStretchWaitMaxNs)) / 1000000.0,
      static_cast<double>(c.completionStretchWaitRing.percentile(0.50)) / 1000000.0,
      static_cast<double>(c.completionStretchWaitRing.percentile(0.95)) / 1000000.0,
      static_cast<double>(c.completionStretchWaitRing.percentile(0.99)) / 1000000.0,
      static_cast<unsigned long long>(load(c.completionBlitOnlyWaits)),
      static_cast<double>(load(c.completionBlitOnlyWaitNs)) / 1000000.0,
      static_cast<double>(load(c.completionBlitOnlyWaitMaxNs)) / 1000000.0,
      static_cast<double>(c.completionBlitOnlyWaitRing.percentile(0.50)) / 1000000.0,
      static_cast<double>(c.completionBlitOnlyWaitRing.percentile(0.95)) / 1000000.0,
      static_cast<double>(c.completionBlitOnlyWaitRing.percentile(0.99)) / 1000000.0,
      static_cast<unsigned long long>(load(c.completionOtherWaits)),
      static_cast<double>(load(c.completionOtherWaitNs)) / 1000000.0,
      static_cast<double>(load(c.completionOtherWaitMaxNs)) / 1000000.0,
      static_cast<double>(c.completionOtherWaitRing.percentile(0.50)) / 1000000.0,
      static_cast<double>(c.completionOtherWaitRing.percentile(0.95)) / 1000000.0,
      static_cast<double>(c.completionOtherWaitRing.percentile(0.99)) / 1000000.0,
      static_cast<unsigned long long>(load(c.syncWaits)),
      static_cast<double>(load(c.syncWaitNs)) / 1000000.0,
      static_cast<double>(load(c.syncWaitMaxNs)) / 1000000.0,
      static_cast<double>(c.syncWaitRing.percentile(0.50)) / 1000000.0,
      static_cast<double>(c.syncWaitRing.percentile(0.95)) / 1000000.0,
      static_cast<double>(c.syncWaitRing.percentile(0.99)) / 1000000.0,
      static_cast<unsigned long long>(load(c.queueWriterWaits)),
      static_cast<double>(load(c.queueWriterWaitNs)) / 1000000.0,
      static_cast<double>(load(c.queueWriterWaitMaxNs)) / 1000000.0,
      static_cast<double>(c.queueWriterWaitRing.percentile(0.50)) / 1000000.0,
      static_cast<double>(c.queueWriterWaitRing.percentile(0.95)) / 1000000.0,
      static_cast<double>(c.queueWriterWaitRing.percentile(0.99)) / 1000000.0,
      static_cast<unsigned long long>(load(c.queueCommitWaits)),
      static_cast<double>(load(c.queueCommitWaitNs)) / 1000000.0,
      static_cast<double>(load(c.queueCommitWaitMaxNs)) / 1000000.0,
      static_cast<double>(c.queueCommitWaitRing.percentile(0.50)) / 1000000.0,
      static_cast<double>(c.queueCommitWaitRing.percentile(0.95)) / 1000000.0,
      static_cast<double>(c.queueCommitWaitRing.percentile(0.99)) / 1000000.0,
      static_cast<unsigned long long>(load(c.queueSequenceWaits)),
      static_cast<double>(load(c.queueSequenceWaitNs)) / 1000000.0,
      static_cast<double>(load(c.queueSequenceWaitMaxNs)) / 1000000.0,
      static_cast<double>(c.queueSequenceWaitRing.percentile(0.50)) / 1000000.0,
      static_cast<double>(c.queueSequenceWaitRing.percentile(0.95)) / 1000000.0,
      static_cast<double>(c.queueSequenceWaitRing.percentile(0.99)) / 1000000.0,
      static_cast<unsigned long long>(load(c.presentBoundaryApplied)),
      static_cast<unsigned long long>(load(c.presentBoundarySkipped)),
      static_cast<unsigned long long>(load(c.presentBoundaryWaits)),
      static_cast<double>(load(c.presentBoundaryWaitNs)) / 1000000.0,
      static_cast<double>(load(c.presentBoundaryWaitMaxNs)) / 1000000.0,
      static_cast<double>(c.presentBoundaryWaitRing.percentile(0.50)) / 1000000.0,
      static_cast<double>(c.presentBoundaryWaitRing.percentile(0.95)) / 1000000.0,
      static_cast<double>(c.presentBoundaryWaitRing.percentile(0.99)) / 1000000.0,
      static_cast<unsigned long long>(load(c.presentEncoded)),
      static_cast<unsigned long long>(load(c.presentSkipped)),
      static_cast<unsigned long long>(load(c.presentFullscreen)),
      static_cast<unsigned long long>(load(c.presentSourceSelections)),
      static_cast<unsigned long long>(load(c.presentSourceExplicit)),
      static_cast<unsigned long long>(load(c.presentSourceCurrentBackBuffer)),
      static_cast<unsigned long long>(load(c.presentSourceChecks)),
      static_cast<unsigned long long>(load(c.presentSourceValid)),
      static_cast<unsigned long long>(load(c.presentSourceMissingSurface)),
      static_cast<unsigned long long>(load(c.presentSourceMissingTexture)),
      static_cast<unsigned long long>(load(c.presentSourceResolve)),
      static_cast<unsigned long long>(load(c.presentSourceInvalidSize)),
      static_cast<unsigned long long>(load(c.presentSourceHandle)),
      static_cast<unsigned long long>(load(c.presentSourceTextureHandle)),
      static_cast<unsigned long long>(load(c.presentSourceWidth)),
      static_cast<unsigned long long>(load(c.presentSourceHeight)),
      static_cast<unsigned long long>(load(c.presentSourceFormat)),
      static_cast<unsigned long long>(load(c.presentSourceSampleCount)),
      static_cast<unsigned long long>(load(c.presentPass)),
      static_cast<unsigned long long>(load(c.presentPassSrcWidth)),
      static_cast<unsigned long long>(load(c.presentPassSrcHeight)),
      static_cast<unsigned long long>(load(c.presentPassDstWidth)),
      static_cast<unsigned long long>(load(c.presentPassDstHeight)),
      static_cast<unsigned long long>(load(c.presentPassDstMaxWidth)),
      static_cast<unsigned long long>(load(c.presentPassDstMaxHeight)),
      static_cast<unsigned long long>(load(c.presentAcquireWaits)),
      static_cast<double>(load(c.presentAcquireWaitNs)) / 1000000.0,
      static_cast<double>(load(c.presentAcquireWaitMaxNs)) / 1000000.0,
      static_cast<double>(c.presentAcquireWaitRing.percentile(0.50)) / 1000000.0,
      static_cast<double>(c.presentAcquireWaitRing.percentile(0.95)) / 1000000.0,
      static_cast<double>(c.presentAcquireWaitRing.percentile(0.99)) / 1000000.0,
      static_cast<unsigned long long>(load(c.presentAcquireSlowWaits)),
      static_cast<unsigned long long>(load(c.presentAsyncAcquireRequests)),
      static_cast<unsigned long long>(load(c.presentAsyncAcquireIssued)),
      static_cast<unsigned long long>(load(c.presentAsyncAcquireFallbacks)),
      static_cast<unsigned long long>(load(c.presentAsyncAcquireWaits)),
      static_cast<double>(load(c.presentAsyncAcquireWaitNs)) / 1000000.0,
      static_cast<double>(load(c.presentAsyncAcquireWaitMaxNs)) / 1000000.0,
      static_cast<double>(c.presentAsyncAcquireWaitRing.percentile(0.50)) / 1000000.0,
      static_cast<double>(c.presentAsyncAcquireWaitRing.percentile(0.95)) / 1000000.0,
      static_cast<double>(c.presentAsyncAcquireWaitRing.percentile(0.99)) / 1000000.0,
      static_cast<unsigned long long>(load(c.presentAsyncAcquireSlowWaits)),
      static_cast<unsigned long long>(load(c.presentTokenWaits)),
      static_cast<double>(load(c.presentTokenWaitNs)) / 1000000.0,
      static_cast<double>(load(c.presentTokenWaitMaxNs)) / 1000000.0,
      static_cast<double>(c.presentTokenWaitRing.percentile(0.50)) / 1000000.0,
      static_cast<double>(c.presentTokenWaitRing.percentile(0.95)) / 1000000.0,
      static_cast<double>(c.presentTokenWaitRing.percentile(0.99)) / 1000000.0,
      static_cast<unsigned long long>(load(c.presentTokenSlowWaits)),
      static_cast<unsigned long long>(load(c.presentPreAcquireRequests)),
      static_cast<unsigned long long>(load(c.presentPreAcquireHits)),
      static_cast<unsigned long long>(load(c.presentPreAcquireMisses)),
      static_cast<double>(load(c.presentPreAcquireWaitNs)) / 1000000.0,
      static_cast<double>(load(c.presentPreAcquireWaitMaxNs)) / 1000000.0,
      static_cast<double>(c.presentPreAcquireWaitRing.percentile(0.50)) / 1000000.0,
      static_cast<double>(c.presentPreAcquireWaitRing.percentile(0.95)) / 1000000.0,
      static_cast<double>(c.presentPreAcquireWaitRing.percentile(0.99)) / 1000000.0,
      static_cast<unsigned long long>(load(c.presentSetPropsWaits)),
      static_cast<double>(load(c.presentSetPropsWaitNs)) / 1000000.0,
      static_cast<double>(c.presentSetPropsWaitRing.percentile(0.50)) / 1000000.0,
      static_cast<double>(c.presentSetPropsWaitRing.percentile(0.95)) / 1000000.0,
      static_cast<double>(c.presentSetPropsWaitRing.percentile(0.99)) / 1000000.0);
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

}  // namespace dxmt9::perf
