#pragma once

// Internal render-pass lifecycle/open seams shared by the draw executor and
// its dedicated Objective-C++ translation units. Source payload views and
// look-ahead spans remain synchronous call-local borrows.

#include "dxmt9_encode_session_storage_internal.hpp"
#include "dxmt9_debug_trace.hpp"
#include "dxmt9_format_convert.hpp"
#include "dxmt9_perf_counters.hpp"
#include "dxmt9_render_pass_close_ledger.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <vector>

namespace dxmt9::encoders {

using encode_session::AttachmentKey;

inline encode_session::AttachmentKey makeAttachmentKey(
    const core::FlatDrawStateRecord& hot) {
  encode_session::AttachmentKey key;
  for (std::size_t i = 0; i < core::kMaxRenderTargets; ++i) {
    key.colorHandles[i] = hot.colorAttachments[i].handle.value;
    key.sampleCount =
        std::max(key.sampleCount, hot.colorAttachments[i].sampleCount);
  }
  key.depthHandle = hot.depthStencil.handle.value;
  key.sampleCount = std::max(key.sampleCount, hot.depthStencil.sampleCount);
  return key;
}

inline encode_session::AttachmentKey makeAttachmentKey(
    const core::ClearDesc& clear) {
  encode_session::AttachmentKey key;
  if (clear.clearColor) {
    for (std::size_t i = 0; i < core::kMaxRenderTargets; ++i) {
      key.colorHandles[i] = clear.colorAttachments[i].handle.value;
      key.sampleCount =
          std::max(key.sampleCount, clear.colorAttachments[i].sampleCount);
    }
  }
  if (clear.clearDepth || clear.clearStencil) {
    key.depthHandle = clear.depthStencil.handle.value;
    key.sampleCount = std::max(key.sampleCount, clear.depthStencil.sampleCount);
  }
  return key;
}

inline WMTWinding frontFaceWinding() {
  return debug::frontFaceCounterClockwise()
             ? WMTWindingCounterClockwise
             : WMTWindingClockwise;
}

inline float metalDepthBiasForDrawState(
    EncodeContext& ctx, const core::FlatDrawStateRecord& hot) {
  const float normalizedBias = std::bit_cast<float>(
      core::flatStateOr(hot.renderStates, core::RS_DEPTH_BIAS, 0u));
  if (normalizedBias == 0.0f || !hot.depthStencil.handle) {
    return 0.0f;
  }
  const auto* surface = ctx.pool.findSurface(hot.depthStencil.handle.value);
  if (!surface || !surface->desc.depthStencil ||
      !convert::formatHasDepthAspect(surface->desc.format)) {
    return 0.0f;
  }
  return convert::toMetalDepthBiasConstant(
      normalizedBias,
      convert::toPixelFormat(surface->desc.format, ctx.limits));
}

struct RenderEncoderGpuAttachment {
  std::array<WMTSampleBufferAttachmentInfo, 1> attachments{};
  core::metalqueue::QueueSubmissionRecord::RenderEncoderGpuSample sample{};
  bool active = false;

  std::span<const WMTSampleBufferAttachmentInfo> span() const {
    return active ? std::span<const WMTSampleBufferAttachmentInfo>(
                        attachments.data(), attachments.size())
                  : std::span<const WMTSampleBufferAttachmentInfo>{};
  }
};

// Coordinator-owned render-pass descriptor and action state. Preparation is
// deliberately separated from Metal encoder creation so the ordinary and
// MTLParallelRenderCommandEncoder lanes consume exactly the same load/store,
// clear, visibility, and attachment policy. No source payload or Metal object
// is retained here.
struct PreparedRenderPass {
  WMTRenderPassInfo info{};
  RenderPassActionSummary actions{};
  LateRenderPassStoreState lateStore{};
  std::uint32_t primaryWidth = 0;
  std::uint32_t primaryHeight = 0;
  bool discardAfterPresent = false;
  bool valid = false;
};

bool prepareRenderPassWithStoreProofLookahead(
    EncodeContext& ctx,
    core::FlatDrawStateView drawState,
    const std::optional<ClearDesc>& clear,
    std::span<const RenderPassStoreProofLookaheadSource> lookaheadSources,
    RenderPassStoreProofActivePass activePass,
    std::span<const WMTSampleBufferAttachmentInfo> sampleBufferAttachments,
    WMT::Buffer visibilityBuffer,
    PreparedRenderPass& prepared);

void configurePreparedRenderPassEncoder(
    EncodeContext& ctx,
    WMT::RenderCommandEncoder& encoder,
    core::FlatDrawStateView drawState,
    const PreparedRenderPass& prepared);

void commitPreparedRenderPassOpen(EncodeContext& ctx,
                                  const PreparedRenderPass& prepared);

struct RenderPassStoreProofSummary {
  perf::RenderPassColorStoreProof color =
      perf::RenderPassColorStoreProof::BlockNullColor;
  perf::RenderPassDepthStoreProof depth =
      perf::RenderPassDepthStoreProof::BlockNullDepth;
  std::uint32_t colorTouchDistance =
      std::numeric_limits<std::uint32_t>::max();
  std::uint32_t depthTouchDistance =
      std::numeric_limits<std::uint32_t>::max();
};

void noteRenderPassFrameClose(RenderPassCloseRecord record) noexcept;

RenderPassStoreProofSummary renderPassStoreProofSummaryForLookahead(
    EncodeContext& ctx,
    std::span<const RenderPassStoreProofLookaheadSource> lookaheadSources,
    const core::FlatDrawStateRecord& hot,
    RenderPassStoreProofActivePass activePass);

WMT::Reference<WMT::RenderCommandEncoder>
beginRenderPassWithStoreProofLookahead(
    EncodeContext& ctx,
    WMT::CommandBuffer& commandBuffer,
    core::FlatDrawStateView drawState,
    const std::optional<core::ClearDesc>& clear,
    std::span<const RenderPassStoreProofLookaheadSource> lookaheadSources,
    RenderPassStoreProofActivePass activePass,
    std::span<const WMTSampleBufferAttachmentInfo> sampleBufferAttachments,
    WMT::Buffer visibilityBuffer,
    RenderPassActionSummary* actionSummary,
    LateRenderPassStoreState* lateStoreState);

namespace encode_session {

class LifecycleRuntime {
public:
  LifecycleRuntime(EncodeContext& ctx,
                   EncodeChunkSessionStorage& storage,
                   EncodeCallState& call,
                   core::CpuReadyTape::SourceRef source,
                   std::uint64_t seqId,
                   std::size_t slotIndex,
                   SessionFinalizeCause finalizeCause =
                       SessionFinalizeCause::FailOrOther);

  core::metalqueue::PublishedCommandRef commandRef(
      std::size_t commandIndex) const noexcept;
  void assertEncoderLifecycleInvariant() const;
  void assertNoActiveEncoder() const;
  RenderEncoderGpuAttachment makeRenderEncoderGpuAttachment(
      core::metalqueue::RenderEncoderGpuPassType passType,
      std::size_t commandIndex,
      std::uint64_t rtHandle,
      std::uint64_t depthHandle,
      std::uint64_t psoHandle = 0);
  RenderEncoderGpuAttachment makeRenderEncoderGpuAttachment(
      core::metalqueue::RenderEncoderGpuPassType passType,
      core::metalqueue::PublishedCommandRef command,
      std::uint64_t rtHandle,
      std::uint64_t depthHandle,
      std::uint64_t psoHandle = 0);
  void recordRenderEncoderGpuAttachment(
      const RenderEncoderGpuAttachment& attachment);
  void flushPendingClear();
  static perf::RenderPassLateStoreAspect perfLateStoreAspect(
      LateRenderPassStoreAspect aspect) noexcept;
  void setLateStoreAction(
      LateRenderPassStoreAttachment& attachment,
      WMTStoreAction action,
      perf::RenderPassLateStoreResolutionCause cause);
  void accountLateStoreActions();
  void resolveLateStoreForClear(const core::ClearCommandView& clear);
  void resolveLateStoreForDraw(core::FlatDrawStateView drawState);
  void resolveLateStoreForStoreCause(
      perf::RenderPassLateStoreResolutionCause cause);
  perf::RenderPassLateStoreResolutionCause lateStoreCloseCause(
      perf::EncoderSplitReason reason) const noexcept;
  // The parallel parent and every child have already ended. Perform the
  // coordinator-owned logical-pass close bookkeeping without attempting to
  // end a child encoder a second time. Sidecar eligibility must already have
  // excluded attachment dumps and visibility work.
  void completeParallelRenderPass(perf::EncoderSplitReason reason);
  void endRender(
      perf::EncoderSplitReason reason = perf::EncoderSplitReason::Final);
  void endBlit();
  void endForCallBoundary();
  bool finalizeIntoSubmission(
      core::metalqueue::QueueSubmissionRecord& record,
      EncodeChunkSessionState* sessionState,
      bool resetSessionAfterPublication);
  bool publishIntoSubmission(
      core::metalqueue::QueueSubmissionRecord& record,
      EncodeChunkSessionState* sessionState,
      bool resetSessionAfterPublication);

private:
  bool canFinalizeIntoSubmission(
      const core::metalqueue::QueueSubmissionRecord& record) const;
  static bool validatePublication(
      const core::metalqueue::QueueSubmissionRecord& record,
      const EncodeChunkSessionState* sessionState);
  void reservePublicationCapacity(
      core::metalqueue::QueueSubmissionRecord& record);
  bool publishPrepared(core::metalqueue::QueueSubmissionRecord& record,
                       EncodeChunkSessionState* sessionState,
                       bool resetSessionAfterPublication);
  static bool validateSources(
      const EncodeChunkSessionState& sessionState,
      const core::metalqueue::QueueSubmissionRecord& record);
  static bool publishSources(
      const EncodeChunkSessionState& sessionState,
      core::metalqueue::QueueSubmissionRecord& record);
  static bool sourcesMatch(
      std::span<const core::metalqueue::QueueCompletionSource> expectedSources,
      std::span<const core::metalqueue::QueueCompletionSource> actualSources);

  template <typename T>
  static void reserveAppendCapacity(const std::vector<T>& source,
                                    std::vector<T>& destination);
  template <typename T>
  static void movePreparedPayload(std::vector<T>& source,
                                  std::vector<T>& destination);

  EncodeContext& ctx_;
  EncodeChunkSessionStorage& storage_;
  EncodeCallState& call_;
  core::CpuReadyTape::SourceRef source_{};
  std::uint64_t seqId_;
  std::size_t slotIndex_;
  SessionFinalizeCause finalizeCause_ = SessionFinalizeCause::FailOrOther;
};

}  // namespace encode_session

}  // namespace dxmt9::encoders
