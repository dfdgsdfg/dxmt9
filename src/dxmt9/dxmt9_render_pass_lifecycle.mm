#import <Metal/Metal.h>

#include "dxmt9_render_pass_internal.hpp"

#include "dxmt9/assert.hpp"
#include "dxmt9_blit_encoders.hpp"
#include "dxmt9_draw_encoder_diagnostics.hpp"
#include "dxmt9_perf_counters.hpp"

#include <algorithm>
#include <limits>
#include <span>
#include <utility>
#include <vector>

namespace dxmt9::encoders {

namespace encode_session {

LifecycleRuntime::LifecycleRuntime(EncodeContext& ctx, EncodeChunkSessionStorage& storage,
                                   EncodeCallState& call, core::CpuReadyTape::SourceRef source,
                                   u64 seqId, std::size_t slotIndex,
                                   SessionFinalizeCause finalizeCause)
    : ctx_(ctx), storage_(storage), call_(call), source_(source), seqId_(seqId),
      slotIndex_(slotIndex), finalizeCause_(finalizeCause) {}

core::metalqueue::PublishedCommandRef
LifecycleRuntime::commandRef(std::size_t commandIndex) const noexcept {
  return core::metalqueue::PublishedCommandRef{
      .source = source_,
      .seqId = seqId_,
      .slotIndex = slotIndex_ <= std::numeric_limits<std::uint32_t>::max()
                       ? static_cast<std::uint32_t>(slotIndex_)
                       : std::numeric_limits<std::uint32_t>::max(),
      .commandIndex = commandIndex <= std::numeric_limits<std::uint32_t>::max()
                          ? static_cast<std::uint32_t>(commandIndex)
                          : std::numeric_limits<std::uint32_t>::max(),
  };
}

void LifecycleRuntime::assertEncoderLifecycleInvariant() const {
  DXMT_ASSERT(!(storage_.encoder.activeRenderEncoder && storage_.encoder.activeBlitEncoder));
  DXMT_ASSERT(storage_.encoder.hasActiveRender ==
              static_cast<bool>(storage_.encoder.activeRenderEncoder));
}

void LifecycleRuntime::assertNoActiveEncoder() const {
  assertEncoderLifecycleInvariant();
  DXMT_ASSERT(!storage_.encoder.activeRenderEncoder);
  DXMT_ASSERT(!storage_.encoder.activeBlitEncoder);
  DXMT_ASSERT(!storage_.encoder.hasActiveRender);
}

RenderEncoderGpuAttachment LifecycleRuntime::makeRenderEncoderGpuAttachment(
    core::metalqueue::RenderEncoderGpuPassType passType, std::size_t commandIndex,
    std::uint64_t rtHandle, std::uint64_t depthHandle, std::uint64_t psoHandle) {
  return makeRenderEncoderGpuAttachment(passType, commandRef(commandIndex), rtHandle, depthHandle,
                                        psoHandle);
}

RenderEncoderGpuAttachment LifecycleRuntime::makeRenderEncoderGpuAttachment(
    core::metalqueue::RenderEncoderGpuPassType passType,
    core::metalqueue::PublishedCommandRef command, std::uint64_t rtHandle,
    std::uint64_t depthHandle, std::uint64_t psoHandle) {
  RenderEncoderGpuAttachment result{};
  if (!storage_.diagnostics.renderEncoderGpuSampleBuffer ||
      storage_.diagnostics.renderEncoderGpuSampleCursor + 1u >=
          storage_.diagnostics.requestedRenderEncoderGpuSamples) {
    return result;
  }
  const auto sourceCommand = encodedCommandIdAtSynchronousEncodeSeam(command);
  if (!sourceCommand) {
    DXMT_ASSERT(false && "GPU sample attribution requires a valid encoded command");
    return result;
  }
  const std::uint32_t startSample = storage_.diagnostics.renderEncoderGpuSampleCursor++;
  const std::uint32_t endSample = storage_.diagnostics.renderEncoderGpuSampleCursor++;
  result.attachments[0] = WMTSampleBufferAttachmentInfo{
      .sample_buffer = storage_.diagnostics.renderEncoderGpuSampleBuffer.handle,
      .start_of_encoder_sample_index = startSample,
      .end_of_encoder_sample_index = endSample,
  };
  result.sample = core::metalqueue::QueueSubmissionRecord::RenderEncoderGpuSample{
      .startIndex = startSample,
      .endIndex = endSample,
      .passType = passType,
      .seqId = command.seqId,
      .slotIndex = command.slotIndex,
      .commandIndex = command.commandIndex,
      .sourceCommand = *sourceCommand,
      .rtHandle = rtHandle,
      .depthHandle = depthHandle,
      .psoHandle = psoHandle,
  };
  result.active = true;
  return result;
}

void LifecycleRuntime::recordRenderEncoderGpuAttachment(
    const RenderEncoderGpuAttachment& attachment) {
  if (attachment.active) {
    storage_.diagnostics.renderEncoderGpuSamples.push_back(attachment.sample);
  }
}

void LifecycleRuntime::flushPendingClear() {
  if (!storage_.pass.pendingClear.has_value()) {
    return;
  }
  const auto& clear = *storage_.pass.pendingClear;
  const auto sampleAttachment = makeRenderEncoderGpuAttachment(
      core::metalqueue::RenderEncoderGpuPassType::Clear, storage_.pass.pendingClearCommand,
      clear.colorAttachments[0].handle.value, clear.depthStencil.handle.value);
  dxmt9::encoders::encodeClearPass(call_.commandBuffer, ctx_.pool, clear, sampleAttachment.span());
  recordRenderEncoderGpuAttachment(sampleAttachment);
  call_.commandBufferHasWork = true;
  storage_.pass.pendingClear.reset();
  storage_.pass.pendingClearCommand = {};
}

perf::RenderPassLateStoreAspect
LifecycleRuntime::perfLateStoreAspect(LateRenderPassStoreAspect aspect) noexcept {
  switch (aspect) {
  case LateRenderPassStoreAspect::Color:
    return perf::RenderPassLateStoreAspect::Color;
  case LateRenderPassStoreAspect::Depth:
    return perf::RenderPassLateStoreAspect::Depth;
  case LateRenderPassStoreAspect::Stencil:
    return perf::RenderPassLateStoreAspect::Stencil;
  }
  return perf::RenderPassLateStoreAspect::Color;
}

void LifecycleRuntime::setLateStoreAction(LateRenderPassStoreAttachment& attachment,
                                          WMTStoreAction action,
                                          perf::RenderPassLateStoreResolutionCause cause) {
  DXMT_ASSERT(attachment.unresolved());
  attachment.action = action;
  if (auto* recorder = ctx_.drawRecorder; recorder && recorder->resolveLateRenderPassStoreAction) {
    recorder->resolveLateRenderPassStoreAction(
        recorder->userdata, static_cast<std::uint8_t>(attachment.aspect), attachment.colorIndex,
        static_cast<std::uint32_t>(action), static_cast<std::uint8_t>(cause));
  }
  if (!suppressRecordedMetalCalls(ctx_)) {
    switch (attachment.aspect) {
    case LateRenderPassStoreAspect::Color:
      storage_.encoder.activeRenderEncoder.setColorStoreAction(action, attachment.colorIndex);
      break;
    case LateRenderPassStoreAspect::Depth:
      storage_.encoder.activeRenderEncoder.setDepthStoreAction(action);
      break;
    case LateRenderPassStoreAspect::Stencil:
      storage_.encoder.activeRenderEncoder.setStencilStoreAction(action);
      break;
    }
  }
  perf::countRenderPassLateStoreResolution(perfLateStoreAspect(attachment.aspect), cause);
}

void LifecycleRuntime::accountLateStoreActions() {
  auto& late = storage_.pass.lateStore;
  for (std::size_t i = 0; i < late.count; ++i) {
    auto& attachment = late.attachments[i];
    if (attachment.accounted) {
      continue;
    }
    DXMT_ASSERT(!attachment.unresolved());
    switch (attachment.aspect) {
    case LateRenderPassStoreAspect::Color:
      perf::countRenderPassStoreActionColor(static_cast<std::uint32_t>(attachment.action));
      if (attachment.colorIndex == 0u) {
        late.summary.color0StoreAction = static_cast<std::uint64_t>(attachment.action);
      }
      break;
    case LateRenderPassStoreAspect::Depth:
      perf::countRenderPassStoreActionDepth(static_cast<std::uint32_t>(attachment.action));
      late.summary.depthStoreAction = static_cast<std::uint64_t>(attachment.action);
      break;
    case LateRenderPassStoreAspect::Stencil:
      perf::countRenderPassStoreActionStencil(static_cast<std::uint32_t>(attachment.action));
      late.summary.stencilStoreAction = static_cast<std::uint64_t>(attachment.action);
      break;
    }
    if (attachment.action == WMTStoreActionStore ||
        attachment.action == WMTStoreActionMultisampleResolve ||
        attachment.action == WMTStoreActionStoreAndMultisampleResolve) {
      perf::countRenderPassTilePreservationBytes(attachment.pixelBytes);
      switch (attachment.aspect) {
      case LateRenderPassStoreAspect::Color:
        late.summary.colorStoreBytes += attachment.pixelBytes;
        break;
      case LateRenderPassStoreAspect::Depth:
        late.summary.depthStoreBytes += attachment.pixelBytes;
        break;
      case LateRenderPassStoreAspect::Stencil:
        late.summary.stencilStoreBytes += attachment.pixelBytes;
        break;
      }
    }
    attachment.accounted = true;
  }
}

void LifecycleRuntime::resolveLateStoreForClear(const core::ClearCommandView& clear) {
  auto& late = storage_.pass.lateStore;
  const bool fullClear = clear.rects.empty();
  for (std::size_t i = 0; i < late.count; ++i) {
    auto& attachment = late.attachments[i];
    if (!attachment.unresolved()) {
      continue;
    }
    bool matchingClear = false;
    if (fullClear) {
      switch (attachment.aspect) {
      case LateRenderPassStoreAspect::Color:
        matchingClear = clear.clearColor && attachment.handle &&
                        clear.colorAttachments[attachment.colorIndex].handle == attachment.handle;
        break;
      case LateRenderPassStoreAspect::Depth:
        matchingClear =
            clear.clearDepth && attachment.handle && clear.depthStencil.handle == attachment.handle;
        break;
      case LateRenderPassStoreAspect::Stencil:
        matchingClear = clear.clearStencil && attachment.handle &&
                        clear.depthStencil.handle == attachment.handle;
        break;
      }
    }
    setLateStoreAction(attachment, matchingClear ? WMTStoreActionDontCare : WMTStoreActionStore,
                       matchingClear ? perf::RenderPassLateStoreResolutionCause::Clear
                                     : perf::RenderPassLateStoreResolutionCause::ClearMismatch);
  }
  accountLateStoreActions();
}

void LifecycleRuntime::resolveLateStoreForDraw(core::FlatDrawStateView drawState) {
  auto& late = storage_.pass.lateStore;
  for (std::size_t i = 0; i < late.count; ++i) {
    auto& attachment = late.attachments[i];
    if (!attachment.unresolved()) {
      continue;
    }
    bool sampled = false;
    if (drawState.hot) {
      for (std::size_t textureIndex = 0; textureIndex < drawState.hot->textures.size();
           ++textureIndex) {
        if ((drawState.hot->textureMask & (1u << textureIndex)) == 0u) {
          continue;
        }
        const core::Handle texture = drawState.hot->textures[textureIndex];
        sampled = texture == attachment.handle ||
                  (attachment.aliasTexture && texture == attachment.aliasTexture);
        if (sampled) {
          break;
        }
      }
    }
    setLateStoreAction(attachment, WMTStoreActionStore,
                       sampled ? perf::RenderPassLateStoreResolutionCause::Sample
                               : perf::RenderPassLateStoreResolutionCause::Draw);
  }
  accountLateStoreActions();
}

void LifecycleRuntime::resolveLateStoreForStoreCause(
    perf::RenderPassLateStoreResolutionCause cause) {
  auto& late = storage_.pass.lateStore;
  for (std::size_t i = 0; i < late.count; ++i) {
    auto& attachment = late.attachments[i];
    if (attachment.unresolved()) {
      setLateStoreAction(attachment, WMTStoreActionStore, cause);
    }
  }
  accountLateStoreActions();
}

perf::RenderPassLateStoreResolutionCause
LifecycleRuntime::lateStoreCloseCause(perf::EncoderSplitReason reason) const noexcept {
  if (reason != perf::EncoderSplitReason::Final) {
    return perf::RenderPassLateStoreResolutionCause::IncompatibleClose;
  }
  switch (finalizeCause_) {
  case SessionFinalizeCause::Drain:
    return perf::RenderPassLateStoreResolutionCause::Drain;
  case SessionFinalizeCause::FailOrOther:
    return perf::RenderPassLateStoreResolutionCause::Error;
  case SessionFinalizeCause::SessionCap:
  case SessionFinalizeCause::Independent:
  case SessionFinalizeCause::Initializer:
  case SessionFinalizeCause::ProducerWait:
    return perf::RenderPassLateStoreResolutionCause::Finalize;
  }
  return perf::RenderPassLateStoreResolutionCause::Error;
}

void LifecycleRuntime::completeParallelRenderPass(
    perf::EncoderSplitReason reason) {
  DXMT_ASSERT(storage_.encoder.hasActiveRender);
  DXMT_ASSERT(!storage_.encoder.activeRenderEncoder);
  DXMT_ASSERT(!storage_.encoder.activeBlitEncoder);
  for (std::size_t i = 0; i < storage_.pass.lateStore.count; ++i) {
    DXMT_ASSERT(!storage_.pass.lateStore.attachments[i].unresolved());
  }
  accountLateStoreActions();
  storage_.diagnostics.activeEncoderBreakdown.recordRenderPassActions(
      storage_.pass.lateStore.summary);
  if (perf::enabled()) {
    const auto& summary = storage_.pass.lateStore.summary;
    noteRenderPassFrameClose(RenderPassCloseRecord{
        .token = storage_.pass.activeInstance,
        .key = RenderPassCloseKey{
            .color0 = storage_.pass.activeKey.colorHandles[0],
            .depth = storage_.pass.activeKey.depthHandle,
            .sampleCount = storage_.pass.activeKey.sampleCount,
        },
        .splitReason = reason,
        .finalizeCause = SessionFinalizeCause::FailOrOther,
        .storeBytes = summary.colorStoreBytes + summary.depthStoreBytes +
            summary.stencilStoreBytes,
    });
  }
  if (auto* recorder = ctx_.drawRecorder; recorder && recorder->endRenderPass) {
    recorder->endRenderPass(recorder->userdata);
  }
  DXMT_ASSERT(!storage_.diagnostics.activeColorAttachmentDump.handle);
  DXMT_ASSERT(!storage_.diagnostics.activeDepthAttachmentDump.handle);
  DXMT_ASSERT(storage_.diagnostics.activeDrawTextureDumps.empty());
  DXMT_ASSERT(!storage_.diagnostics.activeVisibilityScout.has_value());
  perf::countRenderPassEnd(reason);
  storage_.diagnostics.activeEncoderBreakdown.emit(reason);
  storage_.binding.activeStreamIbStaging.begin(false);
  for (auto& handle : storage_.pass.activeColorHandles) {
    if (handle) {
      ctx_.queue.markColorHandleTouched(handle);
      handle = core::Handle{};
    }
  }
  storage_.pass.activeInstance = {};
  storage_.pass.lateStore = {};
  storage_.encoder.hasActiveRender = false;
  storage_.binding.activeDrawStateKey.reset();
  storage_.binding.activeDrawStateUsesPrefetchedPsoLayout = false;
  storage_.binding.textureSamplerShadow.reset();
  assertEncoderLifecycleInvariant();
}

void LifecycleRuntime::endRender(perf::EncoderSplitReason reason) {
  if (!storage_.encoder.activeRenderEncoder) {
    return;
  }
  DXMT_ASSERT(storage_.encoder.hasActiveRender);
  DXMT_ASSERT(!storage_.encoder.activeBlitEncoder);
  resolveLateStoreForStoreCause(lateStoreCloseCause(reason));
  storage_.diagnostics.activeEncoderBreakdown.recordRenderPassActions(
      storage_.pass.lateStore.summary);
  if (perf::enabled()) {
    const auto& summary = storage_.pass.lateStore.summary;
    noteRenderPassFrameClose(RenderPassCloseRecord{
        .token = storage_.pass.activeInstance,
        .key =
            RenderPassCloseKey{
                .color0 = storage_.pass.activeKey.colorHandles[0],
                .depth = storage_.pass.activeKey.depthHandle,
                .sampleCount = storage_.pass.activeKey.sampleCount,
            },
        .splitReason = reason,
        .finalizeCause = reason == perf::EncoderSplitReason::Final
                             ? finalizeCause_
                             : SessionFinalizeCause::FailOrOther,
        .storeBytes = summary.colorStoreBytes + summary.depthStoreBytes + summary.stencilStoreBytes,
    });
  }
  if (auto* recorder = ctx_.drawRecorder; recorder && recorder->endRenderPass) {
    recorder->endRenderPass(recorder->userdata);
  }
  if (!suppressRecordedMetalCalls(ctx_)) {
    storage_.encoder.activeRenderEncoder.popDebugGroup();
    storage_.encoder.activeRenderEncoder.endEncoding();
  }
  maybeEncodeColorAttachmentDump(call_.commandBuffer, ctx_.device,
                                 storage_.diagnostics.activeColorAttachmentDump,
                                 storage_.completion.completionCallbacks);
  maybeEncodeDepthAttachmentDump(call_.commandBuffer, ctx_.device,
                                 storage_.diagnostics.activeDepthAttachmentDump,
                                 storage_.completion.completionCallbacks);
  maybeEncodeDrawTextureDumps(call_.commandBuffer, ctx_.device,
                              storage_.diagnostics.activeDrawTextureDumps,
                              storage_.completion.completionCallbacks);
  if (storage_.diagnostics.activeVisibilityScout) {
    enqueueVisibilityScoutCompletion(*storage_.diagnostics.activeVisibilityScout,
                                     storage_.completion.completionCallbacks);
    storage_.diagnostics.activeVisibilityScout.reset();
  }
  perf::countRenderPassEnd(reason);
  storage_.diagnostics.activeEncoderBreakdown.emit(reason);
  storage_.binding.activeStreamIbStaging.begin(false);
  for (auto& handle : storage_.pass.activeColorHandles) {
    if (handle) {
      ctx_.queue.markColorHandleTouched(handle);
      handle = core::Handle{};
    }
  }
  storage_.diagnostics.activeColorAttachmentDump = {};
  storage_.diagnostics.activeDepthAttachmentDump = {};
  storage_.diagnostics.activeDrawTextureDumps.clear();
  storage_.pass.activeInstance = {};
  storage_.pass.lateStore = {};
  storage_.encoder.activeRenderEncoder = {};
  storage_.encoder.hasActiveRender = false;
  storage_.binding.activeDrawStateKey.reset();
  storage_.binding.activeDrawStateUsesPrefetchedPsoLayout = false;
  storage_.binding.textureSamplerShadow.reset();
  assertEncoderLifecycleInvariant();
}

void LifecycleRuntime::endBlit() {
  if (!storage_.encoder.activeBlitEncoder) {
    return;
  }
  DXMT_ASSERT(!storage_.encoder.activeRenderEncoder);
  DXMT_ASSERT(!storage_.encoder.hasActiveRender);
  storage_.encoder.activeBlitEncoder.endEncoding();
  storage_.encoder.activeBlitEncoder = {};
  assertEncoderLifecycleInvariant();
}

void LifecycleRuntime::endForCallBoundary() {
  flushPendingClear();
  endRender(perf::EncoderSplitReason::Final);
  endBlit();
  assertNoActiveEncoder();
}

bool LifecycleRuntime::finalizeIntoSubmission(core::metalqueue::QueueSubmissionRecord& record,
                                              EncodeChunkSessionState* sessionState,
                                              bool resetSessionAfterPublication) {
  // Keep deterministic contract failures retryable: validation must precede
  // pending-clear flushes, encoder ends, capacity changes, and publication.
  if (!canFinalizeIntoSubmission(record) || !validatePublication(record, sessionState)) {
    return false;
  }
  endForCallBoundary();
  reservePublicationCapacity(record);
  return publishPrepared(record, sessionState, resetSessionAfterPublication);
}

bool LifecycleRuntime::publishIntoSubmission(core::metalqueue::QueueSubmissionRecord& record,
                                             EncodeChunkSessionState* sessionState,
                                             bool resetSessionAfterPublication) {
  if (!canFinalizeIntoSubmission(record) || !validatePublication(record, sessionState)) {
    return false;
  }
  reservePublicationCapacity(record);
  return publishPrepared(record, sessionState, resetSessionAfterPublication);
}

bool LifecycleRuntime::canFinalizeIntoSubmission(
    const core::metalqueue::QueueSubmissionRecord& record) const {
  return call_.commandBuffer &&
         (storage_.commandBufferChainTail == NULL_OBJECT_HANDLE ||
          storage_.commandBufferChainTail == call_.commandBuffer.handle) &&
         !(record.renderEncoderGpuSampleBuffer &&
           storage_.diagnostics.renderEncoderGpuSampleBuffer &&
           record.renderEncoderGpuSampleBuffer.handle !=
               storage_.diagnostics.renderEncoderGpuSampleBuffer.handle) &&
         !(record.metalCapture.has_value() && storage_.diagnostics.metalCaptureRequest.has_value());
}

bool LifecycleRuntime::validatePublication(const core::metalqueue::QueueSubmissionRecord& record,
                                           const EncodeChunkSessionState* sessionState) {
  return !sessionState || validateSources(*sessionState, record);
}

void LifecycleRuntime::reservePublicationCapacity(core::metalqueue::QueueSubmissionRecord& record) {
  reserveAppendCapacity(storage_.diagnostics.renderEncoderGpuSamples,
                        record.renderEncoderGpuSamples);
  reserveAppendCapacity(storage_.completion.postCommitCallbacks, record.postCommitCallbacks);
  reserveAppendCapacity(storage_.completion.completionCallbacks, record.completionCallbacks);
}

bool LifecycleRuntime::publishPrepared(core::metalqueue::QueueSubmissionRecord& record,
                                       EncodeChunkSessionState* sessionState,
                                       bool resetSessionAfterPublication) {
  DXMT_ASSERT(call_.commandBuffer);
  DXMT_ASSERT(!resetSessionAfterPublication || sessionState);
  if (sessionState && !publishSources(*sessionState, record)) {
    return false;
  }
  record.commandBuffer = std::move(call_.commandBuffer);
  if (storage_.diagnostics.metalCaptureRequest.has_value()) {
    record.metalCaptureDevice = WMT::Device{ctx_.device.handle};
    record.metalCapture = std::move(storage_.diagnostics.metalCaptureRequest);
    record.metalCaptureAlreadyStarted = call_.captureAlreadyStartedAtChunkBegin;
  }
  if (!record.renderEncoderGpuSampleBuffer && storage_.diagnostics.renderEncoderGpuSampleBuffer) {
    record.renderEncoderGpuSampleBuffer =
        std::move(storage_.diagnostics.renderEncoderGpuSampleBuffer);
  }
  movePreparedPayload(storage_.diagnostics.renderEncoderGpuSamples, record.renderEncoderGpuSamples);
  movePreparedPayload(storage_.completion.postCommitCallbacks, record.postCommitCallbacks);
  movePreparedPayload(storage_.completion.completionCallbacks, record.completionCallbacks);
  if (resetSessionAfterPublication) {
    resetEncodeChunkSession(*sessionState);
  }
  return true;
}

bool LifecycleRuntime::validateSources(const EncodeChunkSessionState& sessionState,
                                       const core::metalqueue::QueueSubmissionRecord& record) {
  if (!record.completionSpanShadowMatchesSources()) {
    return false;
  }
  const auto sessionSources = sessionState.sources.span();
  if (sessionSources.empty()) {
    return true;
  }
  const auto recordSources = record.explicitCompletionSourceSpan();
  if (recordSources.empty()) {
    core::metalqueue::EncodeSessionSourceList validated;
    return validated.assign(sessionSources);
  }
  return sourcesMatch(sessionSources, recordSources);
}

bool LifecycleRuntime::publishSources(const EncodeChunkSessionState& sessionState,
                                      core::metalqueue::QueueSubmissionRecord& record) {
  if (!record.completionSpanShadowMatchesSources()) {
    return false;
  }
  const auto sessionSources = sessionState.sources.span();
  if (sessionSources.empty()) {
    return true;
  }
  const auto recordSources = record.explicitCompletionSourceSpan();
  if (recordSources.empty()) {
    return record.assignFixedCompletionSources(sessionSources);
  }
  return sourcesMatch(sessionSources, recordSources);
}

bool LifecycleRuntime::sourcesMatch(
    std::span<const core::metalqueue::QueueCompletionSource> expectedSources,
    std::span<const core::metalqueue::QueueCompletionSource> actualSources) {
  return core::metalqueue::queueCompletionSourceSpansExactlyEqual(expectedSources, actualSources);
}

template <typename T>
void LifecycleRuntime::reserveAppendCapacity(const std::vector<T>& source,
                                             std::vector<T>& destination) {
  if (!source.empty() && !destination.empty()) {
    destination.reserve(destination.size() + source.size());
  }
}

template <typename T>
void LifecycleRuntime::movePreparedPayload(std::vector<T>& source, std::vector<T>& destination) {
  if (destination.empty()) {
    destination = std::move(source);
    return;
  }
  for (auto& value : source) {
    destination.push_back(std::move(value));
  }
  source.clear();
}

}  // namespace encode_session

EncodeChunkSessionPassCloseResult
closeEncodeChunkSessionRenderPass(EncodeContext& ctx, EncodeChunkSessionState& sessionState,
                                  core::metalqueue::QueueSubmissionRecord& commandBufferCarrier) {
  if (!sessionState.storage) {
    return EncodeChunkSessionPassCloseResult::InvalidCommandBufferCarrier;
  }
  if (!encode_session::storageHasActiveRender(*sessionState.storage)) {
    return EncodeChunkSessionPassCloseResult::NoActivePass;
  }
  if (!commandBufferCarrier.commandBuffer ||
      sessionState.storage->commandBufferChainTail == NULL_OBJECT_HANDLE ||
      sessionState.storage->commandBufferChainTail != commandBufferCarrier.commandBuffer.handle) {
    return EncodeChunkSessionPassCloseResult::InvalidCommandBufferCarrier;
  }

  encode_session::EncodeCallState call{};
  // Copy the reference deliberately. The carrier remains the sole published
  // owner of this command-buffer chain; LifecycleRuntime only needs a live
  // handle while it executes the exact normal semantic-boundary endRender
  // sequence.
  call.commandBuffer = commandBufferCarrier.commandBuffer;
  encode_session::LifecycleRuntime lifecycle(ctx, *sessionState.storage, call, {},
                                             commandBufferCarrier.seqId,
                                             commandBufferCarrier.slotIndex);
  lifecycle.endRender(perf::EncoderSplitReason::OrderedControl);
  return EncodeChunkSessionPassCloseResult::Closed;
}

bool finalizeEncodeChunkSessionIntoSubmission(EncodeContext& ctx,
                                              EncodeChunkSessionState& sessionState,
                                              core::metalqueue::QueueSubmissionRecord& record,
                                              SessionFinalizeCause cause) {
  DXMT_ASSERT(sessionState.storage);
  encode_session::EncodeCallState call{};
  // Keep the record's ownership intact until all source validation and vector
  // capacity reservations have succeeded.
  call.commandBuffer = record.commandBuffer;
  encode_session::LifecycleRuntime lifecycle(ctx, *sessionState.storage, call, {}, record.seqId,
                                             record.slotIndex, cause);
  const bool finalized = lifecycle.finalizeIntoSubmission(record, &sessionState,
                                                          /*resetSessionAfterPublication=*/true);
  return finalized;
}

}  // namespace dxmt9::encoders
