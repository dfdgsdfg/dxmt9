#include "dxmt9_queue.hpp"

#include "dxmt9/assert.hpp"
#include "dxmt9_compat.hpp"
#include "dxmt9_hud.hpp"
#include "dxmt9_perf_counters.hpp"
#include "dxmt9_signposts.hpp"
#include "../winemetal/Metal.hpp"
#include "util/log/log.hpp"

#include <chrono>
#include <cstdlib>
#include <sstream>

namespace dxmt9::core::metalqueue {

namespace {

using enum dxmt9::core::metalcompat::CompatFlagBits;

u32 compatFlagsForSurface(const std::function<u32(Handle)>& resolveSurfaceFlags, Handle handle) {
  if (!handle || !resolveSurfaceFlags) {
    return 0;
  }
  return resolveSurfaceFlags(handle);
}

u32 compatFlagsForDraw(FlatDrawStateView draw, const std::function<u32(Handle)>& resolveSurfaceFlags) {
  const auto& hot = *draw.hot;
  u32 flags = 0;
  u32 colorTargets = 0;
  for (const auto& attachment : hot.colorAttachments) {
    if (!attachment.handle) {
      continue;
    }
    ++colorTargets;
    flags |= compatFlagsForSurface(resolveSurfaceFlags, attachment.handle);
    if (attachment.sampleCount > 1) {
      flags |= CompatFlagMsaa;
    }
  }
  if (colorTargets > 1) {
    flags |= CompatFlagMrt;
  }
  if (hot.depthStencil.handle && hot.depthStencil.sampleCount > 1) {
    flags |= CompatFlagMsaa;
  }
  if (flatStateOr(hot.renderStates, RS_SRGB_WRITE_ENABLE, 0u) != 0) {
    flags |= CompatFlagSrgb;
  }
  for (const auto& sampler : hot.samplerStates) {
    if (flatStateOr(sampler, SAMP_SRGB_TEXTURE, 0u) != 0) {
      flags |= CompatFlagSrgb;
    }
  }
  if (draw.hasUniformPayload()) {
    const auto& uniformPayload = draw.uniformPayload();
    for (size_t stage = 0; stage < uniformPayload.textureTransforms.size(); ++stage) {
      if (!hot.textures[stage]) {
        continue;
      }
      if (!metalcompat::matrixIsIdentity(uniformPayload.textureTransforms[stage])) {
        flags |= CompatFlagProjected;
        break;
      }
    }
  }
  return flags;
}

u64 shaderVariantHashForDraw(FlatDrawStateView draw) {
  if (!draw.hot || !draw.hasShaderContext()) {
    return 0;
  }
  const auto& hot = *draw.hot;
  const auto& shader = draw.shaderContext();
  u64 hash = shader.vertexShader.hash ^ (shader.pixelShader.hash << 1) ^
             (hot.key.vertexDeclHash << 2) ^ hot.key.renderStateHash ^
             (hot.textureMask << 3) ^ (hot.renderTargetMask << 4);
  hash ^= hot.vertexConstantsHash << 1;
  hash ^= hot.pixelConstantsHash << 2;
  return hash;
}

u32 compatFlagsForClear(const ClearDesc& clear, const std::function<u32(Handle)>& resolveSurfaceFlags) {
  u32 flags = 0;
  u32 colorTargets = 0;
  for (const auto& attachment : clear.colorAttachments) {
    if (!attachment.handle) {
      continue;
    }
    ++colorTargets;
    flags |= compatFlagsForSurface(resolveSurfaceFlags, attachment.handle);
    if (attachment.sampleCount > 1) {
      flags |= CompatFlagMsaa;
    }
  }
  if (colorTargets > 1) {
    flags |= CompatFlagMrt;
  }
  if (clear.depthStencil.handle && clear.depthStencil.sampleCount > 1) {
    flags |= CompatFlagMsaa;
  }
  return flags;
}

u32 compatFlagsForPresent(const SwapDesc& present,
                          Handle sourceHandle,
                          const std::function<u32(Handle)>& resolveSurfaceFlags) {
  u32 flags = compatFlagsForSurface(resolveSurfaceFlags, sourceHandle);
  if (present.multiSampleType != MultiSampleType::None) {
    flags |= CompatFlagMsaa;
  }
  return flags;
}

ChunkObservation makeChunkObservation(const MetalCommandView& command,
                                      const std::function<u32(Handle)>& resolveSurfaceFlags) {
  switch (command.kind) {
    case MetalCommandKind::DrawRun:
      return ChunkObservation{
          .kind = ChunkObservationKind::Draw,
          .compatFlags = command.drawState.hot
              ? compatFlagsForDraw(command.drawState, resolveSurfaceFlags)
              : 0,
          .vertexShaderHash = command.drawState.hasShaderContext()
              ? command.drawState.shaderContext().vertexShader.hash
              : 0,
          .pixelShaderHash = command.drawState.hasShaderContext()
              ? command.drawState.shaderContext().pixelShader.hash
              : 0,
          .shaderVariantHash = shaderVariantHashForDraw(command.drawState),
      };
    case MetalCommandKind::Clear:
      return ChunkObservation{
          .kind = ChunkObservationKind::Draw,
          .compatFlags = command.clear ? compatFlagsForClear(*command.clear, resolveSurfaceFlags) : 0,
      };
    case MetalCommandKind::SurfaceCopy:
    case MetalCommandKind::Readback:
    // R-FORMAT-11: RESZ depth resolve runs on a short-lived helper render
    // encoder with no draws — classify it as a Blit observation like the
    // other helper-encoder surface ops.
    case MetalCommandKind::DepthResolve:
      return ChunkObservation{
          .kind = ChunkObservationKind::Blit,
          .compatFlags = 0,
      };
    case MetalCommandKind::StretchRect:
      return ChunkObservation{
          .kind = ChunkObservationKind::StretchRect,
          .compatFlags = 0,
      };
    case MetalCommandKind::ColorFill:
      return ChunkObservation{
          .kind = ChunkObservationKind::Draw,
          .compatFlags = 0,
      };
    case MetalCommandKind::Present:
      return ChunkObservation{
          .kind = ChunkObservationKind::Present,
          .compatFlags = command.present
              ? compatFlagsForPresent(command.present->present, command.present->presentSource, resolveSurfaceFlags)
              : 0,
      };
  }
  return ChunkObservation{};
}

QueueLifecycleContext makeLifecycleContext(const QueueControllerState& state) {
  return QueueLifecycleContext{
      .writingSlot = state.writingSlot,
      .writeIndex = state.writeIndex,
      .readyCount = state.readyCount,
      .completedQueueCount = state.completedQueueCount,
      .inflightCount = state.inflightCount,
      .completedSeqId = state.completedSeqId,
      .lastCommittedSeqId = state.lastCommittedSeqId,
  };
}

QueueControllerState makeBoundQueueState(
    const QueueLifecycleController::SubmissionBinding& state) {
  return QueueControllerState{
      .writingSlot = state.writingSlot ? *state.writingSlot : std::optional<size_t>{},
      .writeIndex = state.writeIndex ? *state.writeIndex : 0,
      .readyCount = state.readySlots ? state.readySlots->size() : 0,
      .completedQueueCount = state.completedSeqQueue ? state.completedSeqQueue->size() : 0,
      .inflightCount = state.inflightCount ? *state.inflightCount : 0,
      .completedSeqId = state.completedSeqId ? *state.completedSeqId : 0,
      .lastCommittedSeqId = state.lastCommittedSeqId ? *state.lastCommittedSeqId : 0,
      .slots = state.slots,
  };
}

std::optional<ChunkSlot::State> slotStateFor(const QueueControllerState& state,
                                             std::optional<size_t> slotIndex) {
  if (!slotIndex.has_value() || *slotIndex >= state.slots.size()) {
    return std::nullopt;
  }
  return state.slots[*slotIndex].state;
}

size_t slotCommandCountFor(const QueueControllerState& state,
                           std::optional<size_t> slotIndex) {
  if (!slotIndex.has_value() || *slotIndex >= state.slots.size()) {
    return 0;
  }
  return state.slots[*slotIndex].commandCount();
}

bool writerCanProceed(const QueueControllerState& state,
                      std::optional<size_t> slotIndex,
                      size_t inflightLimit) {
  if (!slotIndex.has_value() || *slotIndex >= state.slots.size()) {
    return state.inflightCount < inflightLimit;
  }
  return state.slots[*slotIndex].state == ChunkSlot::State::Free &&
         state.inflightCount < inflightLimit;
}

}  // namespace

CommandBufferDiagnostics summarizeChunk(const ChunkSummaryInput& input) {
  CommandBufferDiagnostics diagnostics;
  diagnostics.seqId = input.seqId;
  diagnostics.slotIndex = input.slotIndex;
  diagnostics.hasDraw = input.hasDraw;
  diagnostics.hasPresent = input.hasPresent;
  diagnostics.hasBlit = input.hasBlit;
  diagnostics.hasStretchRect = input.hasStretchRect;
  diagnostics.frame = input.frame;
  diagnostics.compatFlags = input.compatFlags;
  diagnostics.vertexShaderHash = input.vertexShaderHash;
  diagnostics.pixelShaderHash = input.pixelShaderHash;
  diagnostics.shaderVariantHash = input.shaderVariantHash;
  return diagnostics;
}

CommandBufferDiagnostics summarizeChunk(u64 seqId,
                                        size_t slotIndex,
                                        std::span<const ChunkObservation> observations) {
  ChunkSummaryInput input;
  input.seqId = seqId;
  input.slotIndex = slotIndex;
  for (const auto& observation : observations) {
    switch (observation.kind) {
      case ChunkObservationKind::Draw:
        input.hasDraw = true;
        input.compatFlags |= observation.compatFlags;
        input.vertexShaderHash = observation.vertexShaderHash;
        input.pixelShaderHash = observation.pixelShaderHash;
        input.shaderVariantHash = observation.shaderVariantHash;
        break;
      case ChunkObservationKind::Blit:
        input.hasBlit = true;
        break;
      case ChunkObservationKind::StretchRect:
        input.hasBlit = true;
        input.hasStretchRect = true;
        break;
      case ChunkObservationKind::Present:
        input.hasPresent = true;
        input.compatFlags |= observation.compatFlags;
        break;
    }
  }
  return summarizeChunk(input);
}

CommandBufferDiagnostics summarizeCommands(u64 seqId,
                                           size_t slotIndex,
                                           const ChunkSlot& slot,
                                           const std::function<u32(Handle)>& resolveSurfaceFlags) {
  std::vector<ChunkObservation> observations;
  observations.reserve(slot.commandCount());
  for (std::size_t i = 0; i < slot.commandCount(); ++i) {
    observations.push_back(makeChunkObservation(slot.commandAt(i), resolveSurfaceFlags));
  }
  return summarizeChunk(seqId, slotIndex, std::span<const ChunkObservation>(observations.data(), observations.size()));
}

Handle selectPresentSourceHandle(const SwapDesc& desc, Handle currentBackBuffer) noexcept {
  return desc.sourceSurface ? desc.sourceSurface : currentBackBuffer;
}

CommandBufferDiagnostics QueueLifecycleController::summarizeSubmission(
    u64 seqId,
    size_t slotIndex) const {
  const auto& resolveSurfaceFlags = submissionBinding_.resolveSurfaceFlags;
  if (slotIndex >= submissionBinding_.slots.size()) {
    return CommandBufferDiagnostics{.seqId = seqId, .slotIndex = slotIndex};
  }
  return summarizeCommands(seqId, slotIndex, submissionBinding_.slots[slotIndex], resolveSurfaceFlags);
}

void QueueLifecycleController::observeTransition(const QueueTransitionRecord& record) const {
  switch (classifyTransition(record)) {
    case QueueLifecycleEvent::PresentEnqueue:
      if (record.slotIndex.has_value() && record.present) {
        notePresentEnqueue(record.after, *record.slotIndex, record.eventSeqId, *record.present,
                           record.sourceHandle);
      }
      return;
    case QueueLifecycleEvent::WriterWaitBegin:
      if (record.slotIndex.has_value()) {
        noteWriterWaitBeginIfNeeded(record.before, *record.slotIndex, record.eventSeqId,
                                    record.inflightLimit);
      }
      return;
    case QueueLifecycleEvent::WriterWaitEnd:
      if (record.slotIndex.has_value()) {
        noteWriterWaitEnd(record.after, *record.slotIndex, record.eventSeqId);
      }
      return;
    case QueueLifecycleEvent::WriterAcquire:
      if (record.slotIndex.has_value()) {
        noteWriterAcquire(record.after, *record.slotIndex, record.eventSeqId);
      }
      return;
    case QueueLifecycleEvent::CommitEmpty:
      if (record.slotIndex.has_value()) {
        noteCommitEmpty(record.after, *record.slotIndex, record.eventSeqId);
      }
      return;
    case QueueLifecycleEvent::CommitWaitBegin:
      if (record.slotIndex.has_value()) {
        noteCommitWaitBeginIfNeeded(record.before, *record.slotIndex, record.eventSeqId,
                                    record.inflightLimit);
      }
      return;
    case QueueLifecycleEvent::CommitWaitEnd:
      if (record.slotIndex.has_value()) {
        noteCommitWaitEnd(record.after, *record.slotIndex, record.eventSeqId);
      }
      return;
    case QueueLifecycleEvent::CommitPublish:
      if (record.slotIndex.has_value()) {
        noteCommitPublish(record.after, *record.slotIndex, record.eventSeqId);
      }
      return;
    case QueueLifecycleEvent::EncodeDequeue:
      if (record.slotIndex.has_value()) {
        noteEncodeDequeue(record.after, *record.slotIndex, record.eventSeqId);
      }
      return;
    case QueueLifecycleEvent::EncodeCommit:
      if (record.slotIndex.has_value()) {
        noteEncodeCommit(record.after, *record.slotIndex, record.eventSeqId);
      }
      return;
    case QueueLifecycleEvent::GpuComplete:
      if (record.slotIndex.has_value()) {
        noteGpuComplete(record.after, *record.slotIndex, record.eventSeqId);
      }
      return;
    case QueueLifecycleEvent::FinishInline:
      if (record.slotIndex.has_value()) {
        noteFinishInline(record.after, *record.slotIndex, record.eventSeqId);
      }
      return;
    case QueueLifecycleEvent::FinishDequeue:
      noteFinishDequeue(record.after, record.eventSeqId);
      return;
    case QueueLifecycleEvent::ReclaimFree:
      if (record.slotIndex.has_value()) {
        noteReclaimFree(record.after, *record.slotIndex, record.eventSeqId);
      }
      return;
    case QueueLifecycleEvent::WaitSeqBegin:
      noteWaitSeqBeginIfNeeded(record.before, record.eventSeqId);
      return;
    case QueueLifecycleEvent::WaitSeqEnd:
      noteWaitSeqEnd(record.after, record.eventSeqId);
      return;
  }
}

void QueueLifecycleController::bindTrackedSubmissionState(SubmissionBinding binding) {
  submissionBinding_ = binding;
}

bool QueueLifecycleController::ensureWriterSlot(std::unique_lock<std::mutex>& lock,
                                                size_t inflightLimit) {
  // TLA+: QueueLifecycleRefinement / WriterAcquire.
  auto* writingSlot = submissionBinding_.writingSlot;
  auto* writeIndex = submissionBinding_.writeIndex;
  auto* inflightCount = submissionBinding_.inflightCount;
  auto* writeCv = submissionBinding_.writeCv;
  auto* stop = submissionBinding_.stop;
  if (!writingSlot || !writeIndex || !inflightCount || !writeCv || !stop) {
    return false;
  }

  if (writingSlot->has_value()) {
    return true;
  }

  auto& slots = submissionBinding_.slots;
  const bool waitNeeded =
      slots[*writeIndex].state != ChunkSlot::State::Free || *inflightCount >= inflightLimit;
  if (waitNeeded) {
    observeWriterWait(*writeIndex, slots[*writeIndex].seqId, inflightLimit);
  }
  const auto waitStarted = std::chrono::steady_clock::now();
  writeCv->wait(lock, [&] {
    return *stop || (slots[*writeIndex].state == ChunkSlot::State::Free &&
                     *inflightCount < inflightLimit);
  });
  if (waitNeeded) {
    const auto waitElapsed = std::chrono::steady_clock::now() - waitStarted;
    perf::countQueueWriterWait(static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(waitElapsed).count()));
  }
  if (*stop) {
    return false;
  }
  observeWriterWait(*writeIndex, slots[*writeIndex].seqId, inflightLimit);
  acquireWriterSlot(*writeIndex, slots[*writeIndex].seqId, inflightLimit, [&] {
    slots[*writeIndex].state = ChunkSlot::State::Writing;
    slots[*writeIndex].seqId = 0;
    slots[*writeIndex].clearCommands();
    *writingSlot = *writeIndex;
  });
  return true;
}

void QueueLifecycleController::presentAndCommit(
    std::unique_lock<std::mutex>& lock,
    size_t inflightLimit,
    const SwapDesc& present,
    Handle sourceHandle,
    const std::function<void(const ChunkSlot&)>& onBeforePublish) {
  // TLA+: PresentFrameLatency / CommitPresent.
  if (!ensureWriterSlot(lock, inflightLimit)) {
    return;
  }
  appendPresentCommand(present, sourceHandle);
  (void)commitCurrentChunk(lock, inflightLimit, onBeforePublish);
}

void QueueLifecycleController::flushAndWait(
    std::unique_lock<std::mutex>& lock,
    size_t inflightLimit,
    const std::function<void(const ChunkSlot&)>& onBeforePublish) {
  // TLA+: QueueLifecycleRefinement / CommitPublish then WaitForSequence.
  (void)commitCurrentChunk(lock, inflightLimit, onBeforePublish);

  auto* nextSeqId = submissionBinding_.nextSeqId;
  const u64 targetSeqId = (!nextSeqId || *nextSeqId == 0) ? 0 : *nextSeqId - 1;
  waitForSequence(lock, targetSeqId);
}

bool QueueLifecycleController::commitCurrentChunk(
    std::unique_lock<std::mutex>& lock,
    size_t inflightLimit,
    const std::function<void(const ChunkSlot&)>& onBeforePublish) {
  // TLA+: QueueLifecycleRefinement / CommitEmpty or CommitPublish.
  auto* writingSlot = submissionBinding_.writingSlot;
  auto* writeIndex = submissionBinding_.writeIndex;
  auto* nextSeqId = submissionBinding_.nextSeqId;
  auto* readySlots = submissionBinding_.readySlots;
  auto* inflightCount = submissionBinding_.inflightCount;
  auto* lastCommittedSeqId = submissionBinding_.lastCommittedSeqId;
  auto* writeCv = submissionBinding_.writeCv;
  auto* encodeCv = submissionBinding_.encodeCv;
  auto* stop = submissionBinding_.stop;
  if (!writingSlot || !writeIndex || !nextSeqId || !readySlots || !inflightCount ||
      !lastCommittedSeqId || !writeCv || !stop) {
    return false;
  }
  if (!writingSlot->has_value()) {
    return false;
  }

  auto& slots = submissionBinding_.slots;
  auto& slot = slots[**writingSlot];
  if (slot.commandsEmpty()) {
    const size_t slotIndex = **writingSlot;
    commitEmpty(slotIndex, slot.seqId, [&] {
      slot.state = ChunkSlot::State::Free;
      writingSlot->reset();
    });
    return false;
  }

  const bool waitNeeded = *inflightCount >= inflightLimit;
  if (waitNeeded) {
    observeCommitWait(**writingSlot, slot.seqId, inflightLimit);
  }
  const auto waitStarted = std::chrono::steady_clock::now();
  writeCv->wait(lock, [&] { return *stop || *inflightCount < inflightLimit; });
  if (waitNeeded) {
    const auto waitElapsed = std::chrono::steady_clock::now() - waitStarted;
    perf::countQueueCommitWait(static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(waitElapsed).count()));
  }
  if (*stop) {
    return false;
  }

  const size_t publishedSlotIndex = **writingSlot;
  const u64 publishedSeqId = *nextSeqId;
  observeCommitWait(publishedSlotIndex, slot.seqId, inflightLimit);
  commitPublish(publishedSlotIndex, publishedSeqId, inflightLimit, [&] {
    slot.seqId = (*nextSeqId)++;
    slot.state = ChunkSlot::State::Pending;
    *lastCommittedSeqId = slot.seqId;
    ++(*inflightCount);
    if (onBeforePublish) {
      onBeforePublish(slot);
    }
    readySlots->push_back(publishedSlotIndex);
    writingSlot->reset();
    *writeIndex = (*writeIndex + 1) % slots.size();
  });
  if (encodeCv) {
    encodeCv->notify_one();
  }
  return true;
}

bool QueueLifecycleController::dequeueReadySlot(std::unique_lock<std::mutex>& lock,
                                                size_t& slotIndex,
                                                ChunkSlot& slotCopy) {
  // TLA+: QueueLifecycleRefinement / EncodeDequeue.
  auto* readySlots = submissionBinding_.readySlots;
  auto* encodeCv = submissionBinding_.encodeCv;
  auto* stop = submissionBinding_.stop;
  if (!readySlots || !encodeCv || !stop) {
    return false;
  }

  encodeCv->wait(lock, [&] { return *stop || !readySlots->empty(); });
  if (*stop && readySlots->empty()) {
    return false;
  }

  slotIndex = readySlots->front();
  auto& slot = submissionBinding_.slots[slotIndex];
  encodeDequeue(slotIndex, slot.seqId, [&] {
    readySlots->pop_front();
    slot.state = ChunkSlot::State::Encoding;
  });
  slotCopy = slot;
  return true;
}

bool QueueLifecycleController::runEncodeIteration(
    std::unique_lock<std::mutex>& lock,
    const std::function<std::optional<QueueSubmissionRecord>(size_t, const ChunkSlot&)>& encodeFn,
    const std::function<void(u64)>& onInlineComplete) {
  // TLA+: EncodeDequeue followed by EncodeSubmitToGpu or EncodeCompleteInline.
  size_t slotIndex = 0;
  ChunkSlot slotCopy;
  if (!dequeueReadySlot(lock, slotIndex, slotCopy)) {
    return false;
  }

  lock.unlock();
  std::optional<QueueSubmissionRecord> submission;
  if (encodeFn) {
    submission = encodeFn(slotIndex, slotCopy);
  }
  lock.lock();

  if (submission.has_value()) {
    auto postCommitCallbacks = std::move(submission->postCommitCallbacks);
    enqueueSubmission(*submission);
    lock.unlock();
    for (auto& callback : postCommitCallbacks) {
      if (callback) {
        callback();
      }
    }
  } else {
    completeInlineChunk(slotIndex, slotCopy.seqId);
    if (onInlineComplete) {
      onInlineComplete(slotCopy.seqId);
    }
  }
  return true;
}

void QueueLifecycleController::appendPresentCommand(const SwapDesc& present, Handle sourceHandle) {
  // TLA+: PresentFrameLatency / CommitPresent metadata lane.
  auto* writingSlot = submissionBinding_.writingSlot;
  if (!writingSlot || !writingSlot->has_value()) {
    return;
  }

  const size_t slotIndex = **writingSlot;
  auto& slot = submissionBinding_.slots[slotIndex];
  enqueuePresent(slotIndex, slot.seqId, present, sourceHandle, [&] {
    slot.appendPresent(present, sourceHandle);
  });
}

void QueueLifecycleController::submitEncodedChunk(WMT::Reference<WMT::CommandBuffer> commandBuffer,
                                                  size_t slotIndex,
                                                  u64 seqId,
                                                  const char* context) {
  // TLA+: QueueLifecycleRefinement / EncodeSubmitToGpu.
  QueueSubmissionRecord record;
  record.commandBuffer = std::move(commandBuffer);
  record.slotIndex = slotIndex;
  record.seqId = seqId;
  record.diagnostics = summarizeSubmission(seqId, slotIndex);
  record.context = context;
  enqueueSubmission(record);
}

void QueueLifecycleController::completeInlineChunk(size_t slotIndex, u64 seqId) {
  // TLA+: QueueLifecycleRefinement / EncodeCompleteInline.
  if (slotIndex >= submissionBinding_.slots.size()) {
    return;
  }

  auto& slot = submissionBinding_.slots[slotIndex];
  auto* completedSeqId = submissionBinding_.completedSeqId;
  auto* completedSeqQueue = submissionBinding_.completedSeqQueue;
  finishInline(slotIndex, seqId, [&] {
    // TLA+: QueueLifecycleRefinement / EncodeCompleteInline.
    if (completedSeqId && completedSeqQueue) {
      DXMT_ASSERT(seqId == *completedSeqId + completedSeqQueue->size() + 1);
    }
    slot.state = ChunkSlot::State::Free;
    slot.seqId = 0;
    slot.clearCommands();
    if (completedSeqQueue) {
      completedSeqQueue->push_back(seqId);
    }
  });
  if (submissionBinding_.writeCv) {
    submissionBinding_.writeCv->notify_all();
  }
  if (submissionBinding_.finishCv) {
    submissionBinding_.finishCv->notify_all();
  }
}

bool QueueLifecycleController::drainCompletedSequence(std::unique_lock<std::mutex>& lock,
                                                      u64& seqId) {
  // TLA+: QueueLifecycleRefinement / FinishDequeue.
  auto* completedSeqQueue = submissionBinding_.completedSeqQueue;
  auto* finishCv = submissionBinding_.finishCv;
  auto* stop = submissionBinding_.stop;
  auto* completedSeqId = submissionBinding_.completedSeqId;
  auto* inflightCount = submissionBinding_.inflightCount;
  if (!completedSeqQueue || !finishCv || !stop || !completedSeqId || !inflightCount) {
    return false;
  }

  finishCv->wait(lock, [&] { return *stop || !completedSeqQueue->empty(); });
  if (*stop && completedSeqQueue->empty()) {
    return false;
  }

  seqId = completedSeqQueue->front();
  // TLA+: QueueLifecycleRefinement / FinishDequeue.
  DXMT_ASSERT(seqId == *completedSeqId + 1);
  finishDequeue(seqId, [&] {
    completedSeqQueue->pop_front();
    *completedSeqId = std::max(*completedSeqId, seqId);
    if (*inflightCount > 0) {
      --(*inflightCount);
    }
    auto* completedPresentSeqQueue = submissionBinding_.completedPresentSeqQueue;
    auto* presentCompletedSeqId = submissionBinding_.presentCompletedSeqId;
    if (completedPresentSeqQueue && presentCompletedSeqId) {
      while (!completedPresentSeqQueue->empty() &&
             completedPresentSeqQueue->front() <= *completedSeqId) {
        *presentCompletedSeqId = std::max(*presentCompletedSeqId, completedPresentSeqQueue->front());
        completedPresentSeqQueue->pop_front();
      }
      // TLA+: PresentFrameLatency / PresentCompletionSafety.
      DXMT_ASSERT(*presentCompletedSeqId <= *completedSeqId);
    }
  });
  if (submissionBinding_.finishCv) {
    submissionBinding_.finishCv->notify_all();
  }
  if (submissionBinding_.writeCv) {
    submissionBinding_.writeCv->notify_all();
  }
  if (submissionBinding_.presentCompletedCv) {
    submissionBinding_.presentCompletedCv->notify_all();
  }
  return true;
}

bool QueueLifecycleController::runFinishIteration(std::unique_lock<std::mutex>& lock,
                                                  const std::function<void(u64)>& onAfterFinish) {
  // TLA+: FinishDequeue followed by ReclaimFree.
  u64 seqId = 0;
  if (!drainCompletedSequence(lock, seqId)) {
    return false;
  }
  reclaimCompletedGpuSlots(seqId);
  if (onAfterFinish) {
    onAfterFinish(seqId);
  }
  return true;
}

void QueueLifecycleController::reclaimCompletedGpuSlots(u64 seqId) {
  // TLA+: QueueLifecycleRefinement / ReclaimFree.
  bool reclaimed = false;
  auto& slots = submissionBinding_.slots;
  for (size_t slotIndex = 0; slotIndex < slots.size(); ++slotIndex) {
    auto& slot = slots[slotIndex];
    if (slot.state != ChunkSlot::State::GPU || slot.seqId != seqId) {
      continue;
    }
    reclaimFree(slotIndex, seqId, [&] {
      slot.state = ChunkSlot::State::Free;
      slot.clearCommands();
      slot.seqId = 0;
    });
    reclaimed = true;
  }
  if (reclaimed && submissionBinding_.writeCv) {
    submissionBinding_.writeCv->notify_all();
  }
}

void QueueLifecycleController::waitForSequence(std::unique_lock<std::mutex>& lock,
                                               u64 targetSeqId) {
  // TLA+: QueueLifecycleRefinement / BeginWaitForSequence + EndWaitForSequence.
  auto* completedSeqId = submissionBinding_.completedSeqId;
  auto* finishCv = submissionBinding_.finishCv;
  auto* stop = submissionBinding_.stop;
  if (!completedSeqId || !finishCv || !stop) {
    return;
  }
  if (*completedSeqId < targetSeqId) {
    observeWaitForSequence(targetSeqId);
  }
  const bool waitNeeded = *completedSeqId < targetSeqId;
  const auto waitStarted = std::chrono::steady_clock::now();
  finishCv->wait(lock, [&] { return *stop || *completedSeqId >= targetSeqId; });
  // TLA+: QueueLifecycleRefinement / WaitForSequenceSafety.
  DXMT_ASSERT(*stop || *completedSeqId >= targetSeqId);
  if (waitNeeded) {
    const auto waitElapsed = std::chrono::steady_clock::now() - waitStarted;
    perf::countQueueSequenceWait(static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(waitElapsed).count()));
  }
  if (!*stop) {
    observeWaitForSequence(targetSeqId);
  }
}

void QueueLifecycleController::enqueuePresent(size_t slotIndex,
                                              u64 eventSeqId,
                                              const SwapDesc& present,
                                              Handle sourceHandle,
                                              const std::function<void()>& mutate) {
  transition(QueueTransitionRecord{
                 .slotIndex = slotIndex,
                 .eventSeqId = eventSeqId,
                 .present = &present,
                 .sourceHandle = sourceHandle,
             },
             mutate);
}

void QueueLifecycleController::observeWriterWait(size_t slotIndex,
                                                 u64 eventSeqId,
                                                 size_t inflightLimit) {
  transition(QueueTransitionRecord{
      .slotIndex = slotIndex,
      .eventSeqId = eventSeqId,
      .inflightLimit = inflightLimit,
  });
}

void QueueLifecycleController::acquireWriterSlot(size_t slotIndex,
                                                 u64 eventSeqId,
                                                 size_t inflightLimit,
                                                 const std::function<void()>& mutate) {
  transition(QueueTransitionRecord{
                 .slotIndex = slotIndex,
                 .eventSeqId = eventSeqId,
                 .inflightLimit = inflightLimit,
             },
             mutate);
}

void QueueLifecycleController::commitEmpty(size_t slotIndex,
                                           u64 eventSeqId,
                                           const std::function<void()>& mutate) {
  transition(QueueTransitionRecord{
                 .slotIndex = slotIndex,
                 .eventSeqId = eventSeqId,
             },
             mutate);
}

void QueueLifecycleController::observeCommitWait(size_t slotIndex,
                                                 u64 eventSeqId,
                                                 size_t inflightLimit) {
  transition(QueueTransitionRecord{
      .slotIndex = slotIndex,
      .eventSeqId = eventSeqId,
      .inflightLimit = inflightLimit,
  });
}

void QueueLifecycleController::commitPublish(size_t slotIndex,
                                             u64 eventSeqId,
                                             size_t inflightLimit,
                                             const std::function<void()>& mutate) {
  transition(QueueTransitionRecord{
                 .slotIndex = slotIndex,
                 .eventSeqId = eventSeqId,
                 .inflightLimit = inflightLimit,
             },
             mutate);
}

void QueueLifecycleController::encodeDequeue(size_t slotIndex,
                                             u64 eventSeqId,
                                             const std::function<void()>& mutate) {
  transition(QueueTransitionRecord{
                 .slotIndex = slotIndex,
                 .eventSeqId = eventSeqId,
             },
             mutate);
}

void QueueLifecycleController::enqueueSubmission(QueueSubmissionRecord record) {
  submit(record);
}

void QueueLifecycleController::finishInline(size_t slotIndex,
                                            u64 eventSeqId,
                                            const std::function<void()>& mutate) {
  transition(QueueTransitionRecord{
                 .slotIndex = slotIndex,
                 .eventSeqId = eventSeqId,
             },
             mutate);
}

void QueueLifecycleController::finishDequeue(u64 eventSeqId,
                                             const std::function<void()>& mutate) {
  transition(QueueTransitionRecord{
      .eventSeqId = eventSeqId,
  }, mutate);
}

void QueueLifecycleController::reclaimFree(size_t slotIndex,
                                           u64 eventSeqId,
                                           const std::function<void()>& mutate) {
  transition(QueueTransitionRecord{
                 .slotIndex = slotIndex,
                 .eventSeqId = eventSeqId,
             },
             mutate);
}

void QueueLifecycleController::observeWaitForSequence(u64 targetSeqId) {
  transition(QueueTransitionRecord{
      .eventSeqId = targetSeqId,
  });
}

QueueLifecycleEvent QueueLifecycleController::classifyTransition(const QueueTransitionRecord& record) const {
  if (record.present) {
    return QueueLifecycleEvent::PresentEnqueue;
  }

  if (record.after.completedQueueCount < record.before.completedQueueCount) {
    return QueueLifecycleEvent::FinishDequeue;
  }

  if (!record.slotIndex.has_value()) {
    return record.after.completedSeqId >= record.eventSeqId ? QueueLifecycleEvent::WaitSeqEnd
                                                            : QueueLifecycleEvent::WaitSeqBegin;
  }

  const auto beforeState = slotStateFor(record.before, record.slotIndex);
  const auto afterState = slotStateFor(record.after, record.slotIndex);
  const auto beforeCommandCount = slotCommandCountFor(record.before, record.slotIndex);
  const auto afterCommandCount = slotCommandCountFor(record.after, record.slotIndex);

  if (!record.before.writingSlot.has_value() && record.after.writingSlot.has_value()) {
    return QueueLifecycleEvent::WriterAcquire;
  }

  if (beforeState == ChunkSlot::State::Pending && afterState == ChunkSlot::State::Encoding) {
    return QueueLifecycleEvent::EncodeDequeue;
  }

  if (beforeState == ChunkSlot::State::Encoding && afterState == ChunkSlot::State::GPU) {
    return QueueLifecycleEvent::EncodeCommit;
  }

  if (record.after.completedQueueCount > record.before.completedQueueCount &&
      record.after.inflightCount == record.before.inflightCount) {
    return QueueLifecycleEvent::GpuComplete;
  }

  if (record.after.completedQueueCount > record.before.completedQueueCount &&
      record.after.inflightCount < record.before.inflightCount) {
    return QueueLifecycleEvent::FinishInline;
  }

  if (beforeState == ChunkSlot::State::GPU && afterState == ChunkSlot::State::Free) {
    return QueueLifecycleEvent::ReclaimFree;
  }

  if (record.before.writingSlot.has_value() && !record.after.writingSlot.has_value()) {
    if (record.after.readyCount > record.before.readyCount ||
        record.after.lastCommittedSeqId > record.before.lastCommittedSeqId ||
        record.after.inflightCount > record.before.inflightCount ||
        afterState == ChunkSlot::State::Pending) {
      return QueueLifecycleEvent::CommitPublish;
    }
    return QueueLifecycleEvent::CommitEmpty;
  }

  if (record.before.writingSlot.has_value() || record.after.writingSlot.has_value()) {
    return record.after.inflightCount < record.inflightLimit ? QueueLifecycleEvent::CommitWaitEnd
                                                             : QueueLifecycleEvent::CommitWaitBegin;
  }

  if (beforeCommandCount != afterCommandCount) {
    return QueueLifecycleEvent::PresentEnqueue;
  }

  return writerCanProceed(record.after, record.slotIndex, record.inflightLimit)
             ? QueueLifecycleEvent::WriterWaitEnd
             : QueueLifecycleEvent::WriterWaitBegin;
}

QueueControllerState QueueLifecycleController::currentState() const {
  return makeBoundQueueState(submissionBinding_);
}

#ifndef NDEBUG
void QueueLifecycleController::assertQueueLifecycleInvariants(size_t inflightLimit) const {
  const auto& binding = submissionBinding_;
  const auto slots = binding.slots;
  if (slots.empty()) {
    return;
  }

  const u64 completedSeqId = binding.completedSeqId ? *binding.completedSeqId : 0;
  const u64 lastCommittedSeqId = binding.lastCommittedSeqId ? *binding.lastCommittedSeqId : 0;
  DXMT_ASSERT(completedSeqId <= lastCommittedSeqId);

  if (binding.writeIndex) {
    DXMT_ASSERT(*binding.writeIndex < slots.size());
  }
  if (binding.writingSlot && binding.writingSlot->has_value()) {
    DXMT_ASSERT(**binding.writingSlot < slots.size());
    DXMT_ASSERT(slots[**binding.writingSlot].state == ChunkSlot::State::Writing);
  }

  size_t abstractInflight = 0;
  for (const auto& slot : slots) {
    switch (slot.state) {
      case ChunkSlot::State::Free:
        DXMT_ASSERT(slot.seqId == 0);
        break;
      case ChunkSlot::State::Writing:
        DXMT_ASSERT(slot.seqId == 0);
        break;
      case ChunkSlot::State::Pending:
      case ChunkSlot::State::Encoding:
        DXMT_ASSERT(slot.seqId > 0);
        DXMT_ASSERT(slot.seqId <= lastCommittedSeqId);
        ++abstractInflight;
        break;
      case ChunkSlot::State::GPU:
        DXMT_ASSERT(slot.seqId > 0);
        DXMT_ASSERT(slot.seqId <= lastCommittedSeqId);
        if (slot.seqId > completedSeqId) {
          ++abstractInflight;
        }
        break;
    }
  }

  const size_t effectiveInflightLimit = inflightLimit != 0 ? inflightLimit : slots.size();
  if (binding.inflightCount) {
    DXMT_ASSERT(*binding.inflightCount <= effectiveInflightLimit);
    DXMT_ASSERT(abstractInflight <= *binding.inflightCount);
  }

  if (binding.readySlots) {
    for (size_t i = 0; i < binding.readySlots->size(); ++i) {
      const size_t slotIndex = (*binding.readySlots)[i];
      DXMT_ASSERT(slotIndex < slots.size());
      DXMT_ASSERT(slots[slotIndex].state == ChunkSlot::State::Pending);
      for (size_t j = i + 1; j < binding.readySlots->size(); ++j) {
        DXMT_ASSERT(slotIndex != (*binding.readySlots)[j]);
      }
    }
  }

  if (binding.completedSeqQueue) {
    u64 expectedSeqId = completedSeqId + 1;
    for (const u64 seqId : *binding.completedSeqQueue) {
      DXMT_ASSERT(seqId == expectedSeqId);
      DXMT_ASSERT(seqId <= lastCommittedSeqId);
      ++expectedSeqId;
    }
  }

  if (binding.completedPresentSeqQueue) {
    u64 previousSeqId = completedSeqId;
    for (const u64 seqId : *binding.completedPresentSeqQueue) {
      DXMT_ASSERT(seqId > completedSeqId);
      DXMT_ASSERT(seqId > previousSeqId);
      DXMT_ASSERT(seqId <= lastCommittedSeqId);
      previousSeqId = seqId;
    }
  }

  if (binding.presentCompletedSeqId) {
    DXMT_ASSERT(*binding.presentCompletedSeqId <= completedSeqId);
  }
}

void QueueLifecycleController::assertPendingCompletionInvariantsLocked() const {
  const auto& binding = submissionBinding_;
  const auto slots = binding.slots;
  if (slots.empty()) {
    return;
  }

  const u64 lastCommittedSeqId = binding.lastCommittedSeqId ? *binding.lastCommittedSeqId : 0;
  u64 previousSeqId = 0;
  for (const auto& pending : pendingCompletion_) {
    DXMT_ASSERT(pending.slotIndex < slots.size());
    DXMT_ASSERT(slots[pending.slotIndex].state == ChunkSlot::State::GPU);
    DXMT_ASSERT(slots[pending.slotIndex].seqId == pending.seqId);
    DXMT_ASSERT(pending.seqId > 0);
    DXMT_ASSERT(pending.seqId <= lastCommittedSeqId);
    DXMT_ASSERT(previousSeqId == 0 || pending.seqId > previousSeqId);
    previousSeqId = pending.seqId;
  }
}
#endif

void QueueLifecycleController::transition(QueueTransitionRecord record,
                                          const std::function<void()>& mutate) {
  const auto before = currentState();
  if (mutate) {
    mutate();
  }
  const auto after = currentState();
  record.before = before;
  record.after = after;
#ifndef NDEBUG
  assertQueueLifecycleInvariants(record.inflightLimit);
#endif
  observeTransition(record);
}

void QueueLifecycleController::submit(QueueSubmissionRecord& record) {
  CommandBufferDiagnostics diagnostics = record.diagnostics;
  if (diagnostics.seqId == 0) {
    diagnostics = summarizeSubmission(record.seqId, record.slotIndex);
  }

  const auto beforeCommitState = currentState();
  if (record.slotIndex < submissionBinding_.slots.size()) {
    submissionBinding_.slots[record.slotIndex].state = ChunkSlot::State::GPU;
  }
  const auto afterCommitState = currentState();
#ifndef NDEBUG
  assertQueueLifecycleInvariants();
#endif

  observeTransition(QueueTransitionRecord{
      .before = beforeCommitState,
      .after = afterCommitState,
      .slotIndex = record.slotIndex,
      .eventSeqId = record.seqId,
  });

  if (!record.commandBuffer) {
    return;
  }

  auto commitCommandBuffer = [&] {
    bool captureStarted = false;
    if (record.metalCapture.has_value()) {
      if (record.metalCaptureAlreadyStarted) {
        // Capture was already started at chunk-begin (encodeChunk) so
        // all encoder commands are already in scope. Just remember the
        // active capture so we issue stopCapture after the commit below.
        captureStarted = true;
      } else {
        // Legacy fallback path — should not fire for chunks containing a
        // Present command since encodeChunk pre-starts; kept for safety
        // and for any future capture trigger that bypasses chunk-begin.
        captureStarted = metalcapture::startMetalCapture(record.metalCaptureDevice,
                                                         *record.metalCapture);
      }
    }

    const auto commitStarted = std::chrono::steady_clock::now();
    // M3 — Instruments "commit" interval around the WMT commit call.
    // No-op when no consumer is recording (~5 ns).
    {
      os_log_t signpostLog = dxmt9::signposts::log();
      os_signpost_id_t commitSignpost = os_signpost_id_generate(signpostLog);
      os_signpost_interval_begin(signpostLog, commitSignpost, "commit",
                                 "seq=%llu",
                                 static_cast<unsigned long long>(record.seqId));
      record.commandBuffer.commit();
      os_signpost_interval_end(signpostLog, commitSignpost, "commit",
                               "seq=%llu",
                               static_cast<unsigned long long>(record.seqId));
    }
    perf::countCommandBufferCommitCpuTime(static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - commitStarted).count()));

    if (captureStarted) {
      metalcapture::stopMetalCapture(*record.metalCapture);
    }
  };

  const auto binding = submissionBinding_;
  if (!binding.mutex || !binding.completedSeqQueue) {
    // No binding → just commit; the reference releases when `record` is
    // destroyed by the caller. Covers teardown paths that bypass the
    // finish-thread.
    commitCommandBuffer();
    return;
  }

  CommandBufferDiagnostics preparedDiagnostics = diagnostics;
  auto* diagnosticsController = binding.submissionDiagnostics;
  if (diagnosticsController) {
    preparedDiagnostics = diagnosticsController->prepareQueueSubmission(diagnostics);
  }

  // Commit and hand the record (including the retained WMT::CommandBuffer) to
  // the completion-watcher thread via pendingCompletion_. That thread will
  // call waitUntilCompleted() — the upstream-dxmt finish-thread shape — then
  // push the seqId into completedSeqQueue and fire the transition callbacks.
  commitCommandBuffer();

  {
    std::lock_guard lock(pendingCompletionMutex_);
    PendingCompletion pending;
    pending.commandBuffer = std::move(record.commandBuffer);
    pending.diagnostics = preparedDiagnostics;
    pending.contextValue = record.context ? record.context : "queue";
    pending.slotIndex = record.slotIndex;
    pending.seqId = record.seqId;
    pendingCompletion_.push_back(std::move(pending));
#ifndef NDEBUG
    assertPendingCompletionInvariantsLocked();
#endif
  }
  pendingCompletionCv_.notify_all();
}

bool QueueLifecycleController::processOnePendingCompletion(bool& stop) {
  PendingCompletion pending;
  {
    std::unique_lock<std::mutex> lock(pendingCompletionMutex_);
    pendingCompletionCv_.wait(lock, [&] { return stop || !pendingCompletion_.empty(); });
    if (stop && pendingCompletion_.empty()) {
      return false;
    }
    pending = std::move(pendingCompletion_.front());
    pendingCompletion_.pop_front();
  }

  // Block until GPU completes — upstream dxmt's WaitForFinishThread pattern.
  if (pending.commandBuffer &&
      pending.commandBuffer.status() <= WMTCommandBufferStatusScheduled) {
    const auto started = std::chrono::steady_clock::now();
    pending.commandBuffer.waitUntilCompleted();
    const auto elapsed = std::chrono::steady_clock::now() - started;
    perf::countCompletionWait(
        static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count()),
        pending.diagnostics.hasDraw,
        pending.diagnostics.hasPresent,
        pending.diagnostics.hasBlit,
        pending.diagnostics.hasStretchRect,
        pending.diagnostics.compatFlags,
        pending.diagnostics.vertexShaderHash,
        pending.diagnostics.pixelShaderHash,
        pending.diagnostics.shaderVariantHash);
  }

  const auto binding = submissionBinding_;
  auto* diagnosticsController = binding.submissionDiagnostics;
  if (diagnosticsController && pending.commandBuffer) {
    (void)diagnosticsController->observeQueueSubmission(pending.commandBuffer.handle,
                                                         pending.diagnostics,
                                                         pending.contextValue.c_str());
  }
  if (binding.mutex && binding.completedSeqQueue) {
    std::lock_guard completionLock(*binding.mutex);
    const QueueControllerState before = makeBoundQueueState(binding);
    // TLA+: QueueLifecycleRefinement / GpuComplete.
    DXMT_ASSERT(pending.seqId == (binding.completedSeqId ? *binding.completedSeqId : 0) +
                                   binding.completedSeqQueue->size() + 1);
    binding.completedSeqQueue->push_back(pending.seqId);
    if (pending.diagnostics.hasPresent && binding.completedPresentSeqQueue) {
      binding.completedPresentSeqQueue->push_back(pending.seqId);
    }
    const QueueControllerState after = makeBoundQueueState(binding);
#ifndef NDEBUG
    assertQueueLifecycleInvariants();
#endif
    observeTransition(QueueTransitionRecord{
        .before = before,
        .after = after,
        .slotIndex = pending.slotIndex,
        .eventSeqId = pending.seqId,
    });
    if (binding.finishCv) {
      binding.finishCv->notify_all();
    }
  }
  // pending.commandBuffer is released when `pending` goes out of scope.
  return true;
}

void QueueLifecycleController::notePresentEnqueue(const QueueControllerState& state,
                                                  size_t slotIndex,
                                                  u64 eventSeqId,
                                                  const SwapDesc& present,
                                                  Handle sourceHandle) const {
  if (queueTraceEnabled()) {
    std::ostringstream out;
    out << "[dxmt9-present] enqueue"
        << " hwnd=" << static_cast<unsigned long long>(present.window.value)
        << " source=0x" << std::hex << static_cast<unsigned long long>(sourceHandle.value) << std::dec
        << " size=" << present.width << "x" << present.height;
    emitQueueTraceLine(out.str());
  }
  const auto context = makeLifecycleContext(state);
  traceLifecycleEvent(QueueLifecycleEvent::PresentEnqueue, slotIndex, eventSeqId, context.writingSlot,
                      context.writeIndex, context.readyCount, context.completedQueueCount,
                      context.inflightCount, context.completedSeqId, context.lastCommittedSeqId,
                      state.slots);
}

void QueueLifecycleController::noteWriterWaitBeginIfNeeded(const QueueControllerState& state,
                                                           size_t slotIndex,
                                                           u64 eventSeqId,
                                                           size_t inflightLimit) const {
  if (slotIndex >= state.slots.size()) {
    return;
  }
  if (state.slots[slotIndex].state == ChunkSlot::State::Free && state.inflightCount < inflightLimit) {
    return;
  }
  const auto context = makeLifecycleContext(state);
  traceLifecycleEvent(QueueLifecycleEvent::WriterWaitBegin, slotIndex, eventSeqId, context.writingSlot,
                      context.writeIndex, context.readyCount, context.completedQueueCount,
                      context.inflightCount, context.completedSeqId, context.lastCommittedSeqId,
                      state.slots);
}

void QueueLifecycleController::noteWriterWaitEnd(const QueueControllerState& state,
                                                 size_t slotIndex,
                                                 u64 eventSeqId) const {
  const auto context = makeLifecycleContext(state);
  traceLifecycleEvent(QueueLifecycleEvent::WriterWaitEnd, slotIndex, eventSeqId, context.writingSlot,
                      context.writeIndex, context.readyCount, context.completedQueueCount,
                      context.inflightCount, context.completedSeqId, context.lastCommittedSeqId,
                      state.slots);
}

void QueueLifecycleController::noteWriterAcquire(const QueueControllerState& state,
                                                 size_t slotIndex,
                                                 u64 eventSeqId) const {
  const auto context = makeLifecycleContext(state);
  traceLifecycleEvent(QueueLifecycleEvent::WriterAcquire, slotIndex, eventSeqId, context.writingSlot,
                      context.writeIndex, context.readyCount, context.completedQueueCount,
                      context.inflightCount, context.completedSeqId, context.lastCommittedSeqId,
                      state.slots);
}

void QueueLifecycleController::noteCommitEmpty(const QueueControllerState& state,
                                               size_t slotIndex,
                                               u64 eventSeqId) const {
  const auto context = makeLifecycleContext(state);
  traceLifecycleEvent(QueueLifecycleEvent::CommitEmpty, slotIndex, eventSeqId, context.writingSlot,
                      context.writeIndex, context.readyCount, context.completedQueueCount,
                      context.inflightCount, context.completedSeqId, context.lastCommittedSeqId,
                      state.slots);
}

void QueueLifecycleController::noteCommitWaitBeginIfNeeded(const QueueControllerState& state,
                                                           size_t slotIndex,
                                                           u64 eventSeqId,
                                                           size_t inflightLimit) const {
  if (state.inflightCount < inflightLimit) {
    return;
  }
  const auto context = makeLifecycleContext(state);
  traceLifecycleEvent(QueueLifecycleEvent::CommitWaitBegin, slotIndex, eventSeqId, context.writingSlot,
                      context.writeIndex, context.readyCount, context.completedQueueCount,
                      context.inflightCount, context.completedSeqId, context.lastCommittedSeqId,
                      state.slots);
}

void QueueLifecycleController::noteCommitWaitEnd(const QueueControllerState& state,
                                                 size_t slotIndex,
                                                 u64 eventSeqId) const {
  const auto context = makeLifecycleContext(state);
  traceLifecycleEvent(QueueLifecycleEvent::CommitWaitEnd, slotIndex, eventSeqId, context.writingSlot,
                      context.writeIndex, context.readyCount, context.completedQueueCount,
                      context.inflightCount, context.completedSeqId, context.lastCommittedSeqId,
                      state.slots);
}

void QueueLifecycleController::noteCommitPublish(const QueueControllerState& state,
                                                 size_t slotIndex,
                                                 u64 eventSeqId) const {
  const auto context = makeLifecycleContext(state);
  traceLifecycleEvent(QueueLifecycleEvent::CommitPublish, slotIndex, eventSeqId, context.writingSlot,
                      context.writeIndex, context.readyCount, context.completedQueueCount,
                      context.inflightCount, context.completedSeqId, context.lastCommittedSeqId,
                      state.slots);
}

void QueueLifecycleController::noteEncodeDequeue(const QueueControllerState& state,
                                                 size_t slotIndex,
                                                 u64 eventSeqId) const {
  const auto context = makeLifecycleContext(state);
  traceLifecycleEvent(QueueLifecycleEvent::EncodeDequeue, slotIndex, eventSeqId, context.writingSlot,
                      context.writeIndex, context.readyCount, context.completedQueueCount,
                      context.inflightCount, context.completedSeqId, context.lastCommittedSeqId,
                      state.slots);
}

void QueueLifecycleController::noteEncodeCommit(const QueueControllerState& state,
                                                size_t slotIndex,
                                                u64 eventSeqId) const {
  const auto context = makeLifecycleContext(state);
  traceLifecycleEvent(QueueLifecycleEvent::EncodeCommit, slotIndex, eventSeqId, context.writingSlot,
                      context.writeIndex, context.readyCount, context.completedQueueCount,
                      context.inflightCount, context.completedSeqId, context.lastCommittedSeqId,
                      state.slots);
}

void QueueLifecycleController::noteGpuComplete(const QueueControllerState& state,
                                               size_t slotIndex,
                                               u64 eventSeqId) const {
  const auto context = makeLifecycleContext(state);
  traceLifecycleEvent(QueueLifecycleEvent::GpuComplete, slotIndex, eventSeqId, context.writingSlot,
                      context.writeIndex, context.readyCount, context.completedQueueCount,
                      context.inflightCount, context.completedSeqId, context.lastCommittedSeqId,
                      state.slots);
}

void QueueLifecycleController::noteFinishInline(const QueueControllerState& state,
                                                size_t slotIndex,
                                                u64 eventSeqId) const {
  const auto context = makeLifecycleContext(state);
  traceLifecycleEvent(QueueLifecycleEvent::FinishInline, slotIndex, eventSeqId, context.writingSlot,
                      context.writeIndex, context.readyCount, context.completedQueueCount,
                      context.inflightCount, context.completedSeqId, context.lastCommittedSeqId,
                      state.slots);
}

void QueueLifecycleController::noteFinishDequeue(const QueueControllerState& state,
                                                 u64 eventSeqId) const {
  const auto context = makeLifecycleContext(state);
  traceLifecycleEvent(QueueLifecycleEvent::FinishDequeue, std::nullopt, eventSeqId, context.writingSlot,
                      context.writeIndex, context.readyCount, context.completedQueueCount,
                      context.inflightCount, context.completedSeqId, context.lastCommittedSeqId,
                      state.slots);
}

void QueueLifecycleController::noteReclaimFree(const QueueControllerState& state,
                                               size_t slotIndex,
                                               u64 eventSeqId) const {
  const auto context = makeLifecycleContext(state);
  traceLifecycleEvent(QueueLifecycleEvent::ReclaimFree, slotIndex, eventSeqId, context.writingSlot,
                      context.writeIndex, context.readyCount, context.completedQueueCount,
                      context.inflightCount, context.completedSeqId, context.lastCommittedSeqId,
                      state.slots);
}

void QueueLifecycleController::noteWaitSeqBeginIfNeeded(const QueueControllerState& state,
                                                        u64 targetSeqId) const {
  if (state.completedSeqId >= targetSeqId) {
    return;
  }
  const auto context = makeLifecycleContext(state);
  traceLifecycleEvent(QueueLifecycleEvent::WaitSeqBegin, std::nullopt, targetSeqId, context.writingSlot,
                      context.writeIndex, context.readyCount, context.completedQueueCount,
                      context.inflightCount, context.completedSeqId, context.lastCommittedSeqId,
                      state.slots);
}

void QueueLifecycleController::noteWaitSeqEnd(const QueueControllerState& state,
                                              u64 targetSeqId) const {
  const auto context = makeLifecycleContext(state);
  traceLifecycleEvent(QueueLifecycleEvent::WaitSeqEnd, std::nullopt, targetSeqId, context.writingSlot,
                      context.writeIndex, context.readyCount, context.completedQueueCount,
                      context.inflightCount, context.completedSeqId, context.lastCommittedSeqId,
                      state.slots);
}

std::string CompletionTracker::commandBufferStatusName(WMTCommandBufferStatus status) const {
  switch (status) {
    case WMTCommandBufferStatusNotEnqueued:
      return "not-enqueued";
    case WMTCommandBufferStatusEnqueued:
      return "enqueued";
    case WMTCommandBufferStatusCommitted:
      return "committed";
    case WMTCommandBufferStatusScheduled:
      return "scheduled";
    case WMTCommandBufferStatusCompleted:
      return "completed";
    case WMTCommandBufferStatusError:
      return "error";
  }
  return "unknown";
}

bool CompletionTracker::inspect(obj_handle_t commandBuffer,
                                const CommandBufferDiagnostics& diagnostics,
                                const char* context) {
  if (!commandBuffer) {
    return false;
  }

  WMT::CommandBuffer wrapped{commandBuffer};
  const WMTCommandBufferStatus status = wrapped.status();
  if (queueTraceEnabled()) {
    dxmt9::util::logf(dxmt9::util::LogLevel::Debug, "dxmt9-metal",
                      "%s seq=%llu slot=%zu frame=%u status=%s draw=%d present=%d blit=%d stretch=%d",
                      context,
                      static_cast<unsigned long long>(diagnostics.seqId),
                      diagnostics.slotIndex,
                      diagnostics.frame,
                      commandBufferStatusName(status).c_str(),
                      diagnostics.hasDraw ? 1 : 0,
                      diagnostics.hasPresent ? 1 : 0,
                      diagnostics.hasBlit ? 1 : 0,
                      diagnostics.hasStretchRect ? 1 : 0);
  }

  if (status == WMTCommandBufferStatusError) {
    std::ostringstream summary;
    summary << context << " seq=" << diagnostics.seqId << " status=error";
    WMT::Error error = wrapped.error();
    if (error) {
      summary << " error=" << error.description().getUTF8String();
    }
    lastErrorSummary_ = summary.str();
    dxmt9::util::logLine(dxmt9::util::LogLevel::Error, "dxmt9-metal", lastErrorSummary_);
    // M5 — increment the GPU fault sentinel. Failures are rare so a
    // single counter increment per faulted command buffer is fine and
    // pairs with expected_counters{max=0} on perf probes (L3 gate).
    dxmt9::perf::countGpuCommandBufferError();
  } else if (status == WMTCommandBufferStatusCompleted) {
    // M4 — sample MTLCommandBuffer GPU wall time. Apple Silicon
    // returns nanosecond timestamps via MTLCommandBuffer_property; on
    // older / unsupported drivers the values can be 0 or non-monotonic,
    // so guard before recording.
    const std::uint64_t gpuStart = wrapped.gpuStartTime();
    const std::uint64_t gpuEnd = wrapped.gpuEndTime();
    if (gpuStart > 0 && gpuEnd > gpuStart) {
      dxmt9::perf::countGpuCommandBufferTime(gpuEnd - gpuStart);
    }
    if (diagnostics.hasPresent) {
      lastErrorSummary_.clear();
    }
  } else if (diagnostics.hasPresent) {
    lastErrorSummary_.clear();
  }

  WMT::LogContainer logs = wrapped.logs();
  if (logs) {
    for (WMT::Object logEntry : logs.elements()) {
      WMT::String description = logEntry.description();
      if (!description) {
        continue;
      }
      dxmt9::util::logf(dxmt9::util::LogLevel::Warn,
                        "dxmt9-metal",
                        "%s seq=%llu metal-log=%s",
                        context,
                        static_cast<unsigned long long>(diagnostics.seqId),
                        description.getUTF8String().c_str());
    }
  }

  return diagnostics.hasPresent || status == WMTCommandBufferStatusError;
}

}  // namespace dxmt9::core::metalqueue
