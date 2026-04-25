#include "dxmt9_queue.hpp"

#include "dxmt9_compat.hpp"
#include "dxmt9_hud.hpp"
#include "dxmt9_perf_counters.hpp"
#include "../winemetal/Metal.hpp"
#include "util/config/config.hpp"
#include "util/log/log.hpp"

#include <cstdio>
#include <chrono>
#include <cstdlib>
#include <sstream>

namespace dxmt9::core::metalqueue {

namespace {

using enum dxmt9::core::metalcompat::CompatFlagBits;

const char* slotStateName(QueueSlotState state) {
  switch (state) {
    case QueueSlotState::Free:
      return "free";
    case QueueSlotState::Writing:
      return "writing";
    case QueueSlotState::Pending:
      return "pending";
    case QueueSlotState::Encoding:
      return "encoding";
    case QueueSlotState::GPU:
      return "gpu";
  }
  return "unknown";
}

const char* lifecycleEventName(QueueLifecycleEvent event) {
  switch (event) {
    case QueueLifecycleEvent::PresentEnqueue:
      return "present.enqueue";
    case QueueLifecycleEvent::WriterWaitBegin:
      return "writer.wait.begin";
    case QueueLifecycleEvent::WriterWaitEnd:
      return "writer.wait.end";
    case QueueLifecycleEvent::WriterAcquire:
      return "writer.acquire";
    case QueueLifecycleEvent::CommitEmpty:
      return "commit.empty";
    case QueueLifecycleEvent::CommitWaitBegin:
      return "commit.wait.begin";
    case QueueLifecycleEvent::CommitWaitEnd:
      return "commit.wait.end";
    case QueueLifecycleEvent::CommitPublish:
      return "commit.publish";
    case QueueLifecycleEvent::EncodeDequeue:
      return "encode.dequeue";
    case QueueLifecycleEvent::EncodeCommit:
      return "encode.commit";
    case QueueLifecycleEvent::GpuComplete:
      return "gpu.complete";
    case QueueLifecycleEvent::FinishInline:
      return "finish.inline";
    case QueueLifecycleEvent::FinishDequeue:
      return "finish.dequeue";
    case QueueLifecycleEvent::ReclaimFree:
      return "reclaim.free";
    case QueueLifecycleEvent::WaitSeqBegin:
      return "wait.seq.begin";
    case QueueLifecycleEvent::WaitSeqEnd:
      return "wait.seq.end";
  }
  return "unknown";
}

u32 compatFlagsForSurface(const std::function<u32(Handle)>& resolveSurfaceFlags, Handle handle) {
  if (!handle || !resolveSurfaceFlags) {
    return 0;
  }
  return resolveSurfaceFlags(handle);
}

u32 compatFlagsForDraw(const DrawDesc& draw, const std::function<u32(Handle)>& resolveSurfaceFlags) {
  u32 flags = 0;
  u32 colorTargets = 0;
  for (const auto& attachment : draw.rts.color) {
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
  if (draw.rts.depthStencil.handle && draw.rts.depthStencil.sampleCount > 1) {
    flags |= CompatFlagMsaa;
  }
  if (const auto srgbIt = draw.rs.values.find(RS_SRGB_WRITE_ENABLE);
      srgbIt != draw.rs.values.end() && srgbIt->second != 0) {
    flags |= CompatFlagSrgb;
  }
  for (const auto& sampler : draw.samplers) {
    if (const auto srgbIt = sampler.states.find(SAMP_SRGB_TEXTURE);
        srgbIt != sampler.states.end() && srgbIt->second != 0) {
      flags |= CompatFlagSrgb;
    }
  }
  for (size_t stage = 0; stage < draw.textureTransforms.size(); ++stage) {
    if (!draw.textures[stage].handle) {
      continue;
    }
    if (!metalcompat::matrixIsIdentity(draw.textureTransforms[stage])) {
      flags |= CompatFlagProjected;
      break;
    }
  }
  return flags;
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

ChunkObservation makeChunkObservation(const MetalCommandRecord& command,
                                      const std::function<u32(Handle)>& resolveSurfaceFlags) {
  switch (command.kind) {
    case MetalCommandRecord::Kind::Draw:
      return ChunkObservation{
          .kind = ChunkObservationKind::Draw,
          .compatFlags = compatFlagsForDraw(command.draw, resolveSurfaceFlags),
      };
    case MetalCommandRecord::Kind::Clear:
      return ChunkObservation{
          .kind = ChunkObservationKind::Draw,
          .compatFlags = compatFlagsForClear(command.clear, resolveSurfaceFlags),
      };
    case MetalCommandRecord::Kind::SurfaceCopy:
    case MetalCommandRecord::Kind::StretchRect:
    case MetalCommandRecord::Kind::Readback:
      return ChunkObservation{
          .kind = ChunkObservationKind::Blit,
          .compatFlags = 0,
      };
    case MetalCommandRecord::Kind::ColorFill:
      return ChunkObservation{
          .kind = ChunkObservationKind::Draw,
          .compatFlags = 0,
      };
    case MetalCommandRecord::Kind::Present:
      return ChunkObservation{
          .kind = ChunkObservationKind::Present,
          .compatFlags = compatFlagsForPresent(command.present, command.presentSource, resolveSurfaceFlags),
      };
  }
  return ChunkObservation{};
}

QueueTraceSnapshot makeQueueTraceSnapshot(std::optional<size_t> slotIndex,
                                          u64 eventSeqId,
                                          std::optional<size_t> writingSlot,
                                          size_t writeIndex,
                                          size_t readyCount,
                                          size_t completedQueueCount,
                                          size_t inflightCount,
                                          u64 completedSeqId,
                                          u64 lastCommittedSeqId,
                                          std::span<const ChunkSlot> slots) {
  QueueTraceState traceState;
  traceState.slotIndex = slotIndex;
  traceState.writingSlot = writingSlot;
  traceState.writeIndex = writeIndex;
  traceState.readyCount = readyCount;
  traceState.completedQueueCount = completedQueueCount;
  traceState.inflightCount = inflightCount;
  traceState.completedSeqId = completedSeqId;
  traceState.lastCommittedSeqId = lastCommittedSeqId;
  traceState.eventSeqId = eventSeqId;
  traceState.activeSlots.reserve(slots.size());
  for (size_t i = 0; i < slots.size(); ++i) {
    const auto& slot = slots[i];
    if (slot.state == ChunkSlot::State::Free) {
      continue;
    }
    traceState.activeSlots.push_back(ActiveSlotInfo{
        .index = i,
        .state = static_cast<QueueSlotState>(static_cast<int>(slot.state)),
        .seqId = slot.seqId,
        .commandCount = slot.commands.size(),
    });
  }
  return makeQueueTraceSnapshot(traceState);
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
  return state.slots[*slotIndex].commands.size();
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

bool queueTraceEnabled() {
  static const bool enabled = dxmt9::util::getenvFlag("DXMT_TRACE_QUEUE");
  return enabled;
}

const char* queueTraceFilePath() {
  static const std::string path = dxmt9::util::getenvString("DXMT_TRACE_FILE");
  return path.empty() ? nullptr : path.c_str();
}

u64 queueTraceFromSeq() {
  static const u64 value = dxmt9::util::getenvU64("DXMT_TRACE_QUEUE_FROM").value_or(0);
  return value;
}

void emitQueueTraceLine(const std::string& line) {
  std::fputs(line.c_str(), stderr);
  std::fputc('\n', stderr);
  std::fflush(stderr);
  if (const char* path = queueTraceFilePath()) {
    if (std::FILE* file = std::fopen(path, "a")) {
      std::fputs(line.c_str(), file);
      std::fputc('\n', file);
      std::fclose(file);
    }
  }
}

void emitTextureTraceLine(const std::string& line) {
  emitQueueTraceLine(line);
}

CommandBufferDiagnostics summarizeChunk(const ChunkSummaryInput& input) {
  CommandBufferDiagnostics diagnostics;
  diagnostics.seqId = input.seqId;
  diagnostics.slotIndex = input.slotIndex;
  diagnostics.hasDraw = input.hasDraw;
  diagnostics.hasPresent = input.hasPresent;
  diagnostics.hasBlit = input.hasBlit;
  diagnostics.frame = input.frame;
  diagnostics.compatFlags = input.compatFlags;
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
        break;
      case ChunkObservationKind::Blit:
        input.hasBlit = true;
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
                                           std::span<const MetalCommandRecord> commands,
                                           const std::function<u32(Handle)>& resolveSurfaceFlags) {
  std::vector<ChunkObservation> observations;
  observations.reserve(commands.size());
  for (const auto& command : commands) {
    observations.push_back(makeChunkObservation(command, resolveSurfaceFlags));
  }
  return summarizeChunk(seqId, slotIndex, std::span<const ChunkObservation>(observations.data(), observations.size()));
}

CommandBufferDiagnostics QueueLifecycleController::summarizeSubmission(
    u64 seqId,
    size_t slotIndex,
    std::span<const MetalCommandRecord> commands) const {
  const auto& resolveSurfaceFlags = submissionBinding_.resolveSurfaceFlags;
  return summarizeCommands(seqId, slotIndex, commands, resolveSurfaceFlags);
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
    slots[*writeIndex].commands.clear();
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
  (void)commitCurrentChunk(lock, inflightLimit, onBeforePublish);

  auto* nextSeqId = submissionBinding_.nextSeqId;
  const u64 targetSeqId = (!nextSeqId || *nextSeqId == 0) ? 0 : *nextSeqId - 1;
  waitForSequence(lock, targetSeqId);
}

bool QueueLifecycleController::commitCurrentChunk(
    std::unique_lock<std::mutex>& lock,
    size_t inflightLimit,
    const std::function<void(const ChunkSlot&)>& onBeforePublish) {
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
  if (slot.commands.empty()) {
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
    enqueueSubmission(*submission);
  } else {
    completeInlineChunk(slotIndex, slotCopy.seqId);
    if (onInlineComplete) {
      onInlineComplete(slotCopy.seqId);
    }
  }
  return true;
}

void QueueLifecycleController::appendPresentCommand(const SwapDesc& present, Handle sourceHandle) {
  auto* writingSlot = submissionBinding_.writingSlot;
  if (!writingSlot || !writingSlot->has_value()) {
    return;
  }

  const size_t slotIndex = **writingSlot;
  auto& slot = submissionBinding_.slots[slotIndex];
  enqueuePresent(slotIndex, slot.seqId, present, sourceHandle, [&] {
    MetalCommandRecord op;
    op.kind = MetalCommandRecord::Kind::Present;
    op.present = present;
    op.presentSource = sourceHandle;
    slot.commands.push_back(std::move(op));
  });
}

void QueueLifecycleController::submitEncodedChunk(WMT::Reference<WMT::CommandBuffer> commandBuffer,
                                                  size_t slotIndex,
                                                  u64 seqId,
                                                  std::span<const MetalCommandRecord> commands,
                                                  const char* context) {
  QueueSubmissionRecord record;
  record.commandBuffer = std::move(commandBuffer);
  record.slotIndex = slotIndex;
  record.seqId = seqId;
  record.commands = commands;
  record.context = context;
  enqueueSubmission(record);
}

void QueueLifecycleController::completeInlineChunk(size_t slotIndex, u64 seqId) {
  if (slotIndex >= submissionBinding_.slots.size()) {
    return;
  }

  auto& slot = submissionBinding_.slots[slotIndex];
  auto* completedSeqId = submissionBinding_.completedSeqId;
  auto* inflightCount = submissionBinding_.inflightCount;
  auto* completedSeqQueue = submissionBinding_.completedSeqQueue;
  finishInline(slotIndex, seqId, [&] {
    slot.state = ChunkSlot::State::Free;
    slot.seqId = seqId;
    slot.commands.clear();
    if (inflightCount && *inflightCount > 0) {
      --(*inflightCount);
    }
    if (completedSeqId) {
      *completedSeqId = std::max(*completedSeqId, seqId);
    }
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
  finishDequeue(seqId, [&] {
    completedSeqQueue->pop_front();
    *completedSeqId = std::max(*completedSeqId, seqId);
    if (*inflightCount > 0) {
      --(*inflightCount);
    }
  });
  if (submissionBinding_.finishCv) {
    submissionBinding_.finishCv->notify_all();
  }
  if (submissionBinding_.writeCv) {
    submissionBinding_.writeCv->notify_all();
  }
  return true;
}

bool QueueLifecycleController::runFinishIteration(std::unique_lock<std::mutex>& lock,
                                                  const std::function<void(u64)>& onAfterFinish) {
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
  bool reclaimed = false;
  auto& slots = submissionBinding_.slots;
  for (size_t slotIndex = 0; slotIndex < slots.size(); ++slotIndex) {
    auto& slot = slots[slotIndex];
    if (slot.state != ChunkSlot::State::GPU || slot.seqId != seqId) {
      continue;
    }
    reclaimFree(slotIndex, seqId, [&] {
      slot.state = ChunkSlot::State::Free;
      slot.commands.clear();
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

void QueueLifecycleController::transition(QueueTransitionRecord record,
                                          const std::function<void()>& mutate) {
  const auto before = currentState();
  if (mutate) {
    mutate();
  }
  const auto after = currentState();
  record.before = before;
  record.after = after;
  observeTransition(record);
}

void QueueLifecycleController::submit(QueueSubmissionRecord& record) {
  const auto diagnostics = summarizeSubmission(record.seqId, record.slotIndex, record.commands);

  const auto beforeCommitState = currentState();
  if (record.slotIndex < submissionBinding_.slots.size()) {
    submissionBinding_.slots[record.slotIndex].state = ChunkSlot::State::GPU;
  }
  const auto afterCommitState = currentState();

  observeTransition(QueueTransitionRecord{
      .before = beforeCommitState,
      .after = afterCommitState,
      .slotIndex = record.slotIndex,
      .eventSeqId = record.seqId,
  });

  if (!record.commandBuffer) {
    return;
  }

  const auto binding = submissionBinding_;
  if (!binding.mutex || !binding.completedSeqQueue) {
    // No binding → just commit; the reference releases when `record` is
    // destroyed by the caller. Covers teardown paths that bypass the
    // finish-thread.
    record.commandBuffer.commit();
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
  record.commandBuffer.commit();

  {
    std::lock_guard lock(pendingCompletionMutex_);
    PendingCompletion pending;
    pending.commandBuffer = std::move(record.commandBuffer);
    pending.diagnostics = preparedDiagnostics;
    pending.contextValue = record.context ? record.context : "queue";
    pending.slotIndex = record.slotIndex;
    pending.seqId = record.seqId;
    pendingCompletion_.push_back(std::move(pending));
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
        pending.diagnostics.hasBlit);
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
    binding.completedSeqQueue->push_back(pending.seqId);
    const QueueControllerState after = makeBoundQueueState(binding);
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

QueueTraceSnapshot makeQueueTraceSnapshot(const QueueTraceState& state) {
  QueueTraceSnapshot snapshot;
  snapshot.slotIndex = state.slotIndex;
  snapshot.writingSlot = state.writingSlot;
  snapshot.writeIndex = state.writeIndex;
  snapshot.readyCount = state.readyCount;
  snapshot.completedQueueCount = state.completedQueueCount;
  snapshot.inflightCount = state.inflightCount;
  snapshot.completedSeqId = state.completedSeqId;
  snapshot.lastCommittedSeqId = state.lastCommittedSeqId;
  snapshot.eventSeqId = state.eventSeqId;
  snapshot.activeSlots = state.activeSlots;
  return snapshot;
}

bool shouldTraceQueue(const QueueTraceSnapshot& snapshot) {
  if (!queueTraceEnabled()) {
    return false;
  }
  const u64 threshold = queueTraceFromSeq();
  if (threshold == 0) {
    return true;
  }
  if (snapshot.eventSeqId >= threshold && snapshot.eventSeqId != 0) {
    return true;
  }
  for (const auto& slot : snapshot.activeSlots) {
    if (slot.seqId >= threshold && slot.seqId != 0) {
      return true;
    }
  }
  if (snapshot.completedSeqId >= threshold || snapshot.lastCommittedSeqId >= threshold) {
    return true;
  }
  return false;
}

std::string formatActiveSlots(const QueueTraceSnapshot& snapshot) {
  std::ostringstream out;
  bool first = true;
  for (const auto& slot : snapshot.activeSlots) {
    if (!first) {
      out << ' ';
    }
    first = false;
    out << slot.index << ':' << slotStateName(slot.state);
    if (slot.seqId != 0) {
      out << '#' << slot.seqId;
    }
    if (slot.commandCount != 0) {
      out << '/' << slot.commandCount;
    }
  }
  if (first) {
    out << "none";
  }
  return out.str();
}

void traceQueueEvent(const char* event, const QueueTraceSnapshot& snapshot, const char* extra) {
  if (!shouldTraceQueue(snapshot)) {
    return;
  }
  std::ostringstream out;
  out << "[dxmt9-queue] " << event
      << " seq=" << static_cast<unsigned long long>(snapshot.eventSeqId)
      << " slot=";
  if (snapshot.slotIndex.has_value()) {
    out << *snapshot.slotIndex;
  } else {
    out << '-';
  }
  out << " writeIndex=" << snapshot.writeIndex
      << " writing=";
  if (snapshot.writingSlot.has_value()) {
    out << *snapshot.writingSlot;
  } else {
    out << '-';
  }
  out << " ready=" << snapshot.readyCount
      << " completedQ=" << snapshot.completedQueueCount
      << " inflight=" << snapshot.inflightCount
      << " completed=" << static_cast<unsigned long long>(snapshot.completedSeqId)
      << " lastCommitted=" << static_cast<unsigned long long>(snapshot.lastCommittedSeqId)
      << " slots=[" << formatActiveSlots(snapshot) << "]";
  if (extra && extra[0] != '\0') {
    out << ' ' << extra;
  }
  emitQueueTraceLine(out.str());
}

void traceLifecycleEvent(QueueLifecycleEvent event,
                         std::optional<size_t> slotIndex,
                         u64 eventSeqId,
                         std::optional<size_t> writingSlot,
                         size_t writeIndex,
                         size_t readyCount,
                         size_t completedQueueCount,
                         size_t inflightCount,
                         u64 completedSeqId,
                         u64 lastCommittedSeqId,
                         std::span<const ChunkSlot> slots,
                         const char* extra) {
  traceQueueSlotsEvent(lifecycleEventName(event), slotIndex, eventSeqId, writingSlot, writeIndex,
                       readyCount, completedQueueCount, inflightCount, completedSeqId,
                       lastCommittedSeqId, slots, extra);
}

void traceQueueSlotsEvent(const char* event,
                          std::optional<size_t> slotIndex,
                          u64 eventSeqId,
                          std::optional<size_t> writingSlot,
                          size_t writeIndex,
                          size_t readyCount,
                          size_t completedQueueCount,
                          size_t inflightCount,
                          u64 completedSeqId,
                          u64 lastCommittedSeqId,
                          std::span<const ChunkSlot> slots,
                          const char* extra) {
  traceQueueEvent(event, makeQueueTraceSnapshot(slotIndex, eventSeqId, writingSlot, writeIndex, readyCount,
                                                completedQueueCount, inflightCount, completedSeqId,
                                                lastCommittedSeqId, slots),
                  extra);
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
                      "%s seq=%llu slot=%zu frame=%u status=%s draw=%d present=%d blit=%d",
                      context,
                      static_cast<unsigned long long>(diagnostics.seqId),
                      diagnostics.slotIndex,
                      diagnostics.frame,
                      commandBufferStatusName(status).c_str(),
                      diagnostics.hasDraw ? 1 : 0,
                      diagnostics.hasPresent ? 1 : 0,
                      diagnostics.hasBlit ? 1 : 0);
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
