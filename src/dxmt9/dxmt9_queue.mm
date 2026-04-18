#import <objc/message.h>

#include "dxmt9_queue.hpp"

#include "dxmt9_compat.hpp"
#include "dxmt9_hud.hpp"
#include "util/config/config.hpp"
#include "util/log/log.hpp"

#include <cstdio>
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
    std::span<const MetalCommandRecord> commands,
    const std::function<u32(Handle)>& resolveSurfaceFlags) const {
  return summarizeCommands(seqId, slotIndex, commands, resolveSurfaceFlags);
}

void QueueLifecycleController::observeTransition(const QueueTransitionRecord& record) const {
  switch (record.event) {
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

QueueControllerState QueueLifecycleController::currentState() const {
  return makeBoundQueueState(submissionBinding_);
}

void QueueLifecycleController::presentEnqueue(size_t slotIndex,
                                              u64 eventSeqId,
                                              const SwapDesc& present,
                                              Handle sourceHandle,
                                              const std::function<void()>& mutate) {
  const auto before = currentState();
  if (mutate) {
    mutate();
  }
  const auto after = currentState();
  observeTransition(QueueTransitionRecord{
      .event = QueueLifecycleEvent::PresentEnqueue,
      .before = before,
      .after = after,
      .slotIndex = slotIndex,
      .eventSeqId = eventSeqId,
      .present = &present,
      .sourceHandle = sourceHandle,
  });
}

void QueueLifecycleController::writerWaitBeginIfNeeded(size_t slotIndex,
                                                       u64 eventSeqId,
                                                       size_t inflightLimit) const {
  const auto state = currentState();
  observeTransition(QueueTransitionRecord{
      .event = QueueLifecycleEvent::WriterWaitBegin,
      .before = state,
      .after = state,
      .slotIndex = slotIndex,
      .eventSeqId = eventSeqId,
      .inflightLimit = inflightLimit,
  });
}

void QueueLifecycleController::writerWaitEnd(size_t slotIndex, u64 eventSeqId) const {
  const auto state = currentState();
  observeTransition(QueueTransitionRecord{
      .event = QueueLifecycleEvent::WriterWaitEnd,
      .before = state,
      .after = state,
      .slotIndex = slotIndex,
      .eventSeqId = eventSeqId,
  });
}

void QueueLifecycleController::writerAcquire(size_t slotIndex,
                                             u64 eventSeqId,
                                             const std::function<void()>& mutate) {
  const auto before = currentState();
  if (mutate) {
    mutate();
  }
  const auto after = currentState();
  observeTransition(QueueTransitionRecord{
      .event = QueueLifecycleEvent::WriterAcquire,
      .before = before,
      .after = after,
      .slotIndex = slotIndex,
      .eventSeqId = eventSeqId,
  });
}

void QueueLifecycleController::commitEmpty(size_t slotIndex,
                                           u64 eventSeqId,
                                           const std::function<void()>& mutate) {
  const auto before = currentState();
  if (mutate) {
    mutate();
  }
  const auto after = currentState();
  observeTransition(QueueTransitionRecord{
      .event = QueueLifecycleEvent::CommitEmpty,
      .before = before,
      .after = after,
      .slotIndex = slotIndex,
      .eventSeqId = eventSeqId,
  });
}

void QueueLifecycleController::commitWaitBeginIfNeeded(size_t slotIndex,
                                                       u64 eventSeqId,
                                                       size_t inflightLimit) const {
  const auto state = currentState();
  observeTransition(QueueTransitionRecord{
      .event = QueueLifecycleEvent::CommitWaitBegin,
      .before = state,
      .after = state,
      .slotIndex = slotIndex,
      .eventSeqId = eventSeqId,
      .inflightLimit = inflightLimit,
  });
}

void QueueLifecycleController::commitWaitEnd(size_t slotIndex, u64 eventSeqId) const {
  const auto state = currentState();
  observeTransition(QueueTransitionRecord{
      .event = QueueLifecycleEvent::CommitWaitEnd,
      .before = state,
      .after = state,
      .slotIndex = slotIndex,
      .eventSeqId = eventSeqId,
  });
}

void QueueLifecycleController::commitPublish(size_t slotIndex,
                                             u64 eventSeqId,
                                             const std::function<void()>& mutate) {
  const auto before = currentState();
  if (mutate) {
    mutate();
  }
  const auto after = currentState();
  observeTransition(QueueTransitionRecord{
      .event = QueueLifecycleEvent::CommitPublish,
      .before = before,
      .after = after,
      .slotIndex = slotIndex,
      .eventSeqId = eventSeqId,
  });
}

void QueueLifecycleController::encodeDequeue(size_t slotIndex,
                                             u64 eventSeqId,
                                             const std::function<void()>& mutate) {
  const auto before = currentState();
  if (mutate) {
    mutate();
  }
  const auto after = currentState();
  observeTransition(QueueTransitionRecord{
      .event = QueueLifecycleEvent::EncodeDequeue,
      .before = before,
      .after = after,
      .slotIndex = slotIndex,
      .eventSeqId = eventSeqId,
  });
}

void QueueLifecycleController::commitTrackedSubmission(
    id<MTLCommandBuffer> commandBuffer,
    size_t slotIndex,
    u64 seqId,
    std::span<const MetalCommandRecord> commands,
    metalhud::SubmissionDiagnosticsController& submissionDiagnostics,
    const std::function<u32(Handle)>& resolveSurfaceFlags,
    const char* context) {
  const auto diagnostics = summarizeSubmission(seqId, slotIndex, commands, resolveSurfaceFlags);

  const auto beforeCommitState = currentState();
  if (slotIndex < submissionBinding_.slots.size()) {
    submissionBinding_.slots[slotIndex].state = ChunkSlot::State::GPU;
  }
  const auto afterCommitState = currentState();

  observeTransition(QueueTransitionRecord{
      .event = QueueLifecycleEvent::EncodeCommit,
      .before = beforeCommitState,
      .after = afterCommitState,
      .slotIndex = slotIndex,
      .eventSeqId = seqId,
  });

  if (!commandBuffer) {
    return;
  }

  const auto binding = submissionBinding_;
  if (!binding.mutex || !binding.completedSeqQueue) {
    return;
  }

  const auto preparedDiagnostics = submissionDiagnostics.prepareQueueSubmission(diagnostics);
  auto* self = this;
  auto* diagnosticsController = &submissionDiagnostics;
  const std::string contextValue = context ? context : "queue";
  const auto submissionSlotIndex = slotIndex;
  const auto submissionSeqId = seqId;
  [commandBuffer addCompletedHandler:^(id<MTLCommandBuffer> buffer) {
    (void)diagnosticsController->observeQueueSubmission(buffer, preparedDiagnostics, contextValue.c_str());
    std::lock_guard completionLock(*binding.mutex);
    const QueueControllerState before = makeBoundQueueState(binding);
    binding.completedSeqQueue->push_back(submissionSeqId);
    const QueueControllerState after = makeBoundQueueState(binding);
    self->observeTransition(QueueTransitionRecord{
        .event = QueueLifecycleEvent::GpuComplete,
        .before = before,
        .after = after,
        .slotIndex = submissionSlotIndex,
        .eventSeqId = submissionSeqId,
    });
    if (binding.finishCv) {
      binding.finishCv->notify_all();
    }
  }];
}

void QueueLifecycleController::finishInline(size_t slotIndex,
                                            u64 eventSeqId,
                                            const std::function<void()>& mutate) {
  const auto before = currentState();
  if (mutate) {
    mutate();
  }
  const auto after = currentState();
  observeTransition(QueueTransitionRecord{
      .event = QueueLifecycleEvent::FinishInline,
      .before = before,
      .after = after,
      .slotIndex = slotIndex,
      .eventSeqId = eventSeqId,
  });
}

void QueueLifecycleController::finishDequeue(u64 eventSeqId,
                                             const std::function<void()>& mutate) {
  const auto before = currentState();
  if (mutate) {
    mutate();
  }
  const auto after = currentState();
  observeTransition(QueueTransitionRecord{
      .event = QueueLifecycleEvent::FinishDequeue,
      .before = before,
      .after = after,
      .eventSeqId = eventSeqId,
  });
}

void QueueLifecycleController::reclaimFree(size_t slotIndex,
                                           u64 eventSeqId,
                                           const std::function<void()>& mutate) {
  const auto before = currentState();
  if (mutate) {
    mutate();
  }
  const auto after = currentState();
  observeTransition(QueueTransitionRecord{
      .event = QueueLifecycleEvent::ReclaimFree,
      .before = before,
      .after = after,
      .slotIndex = slotIndex,
      .eventSeqId = eventSeqId,
  });
}

void QueueLifecycleController::waitSeqBeginIfNeeded(u64 targetSeqId) const {
  const auto state = currentState();
  observeTransition(QueueTransitionRecord{
      .event = QueueLifecycleEvent::WaitSeqBegin,
      .before = state,
      .after = state,
      .eventSeqId = targetSeqId,
  });
}

void QueueLifecycleController::waitSeqEnd(u64 targetSeqId) const {
  const auto state = currentState();
  observeTransition(QueueTransitionRecord{
      .event = QueueLifecycleEvent::WaitSeqEnd,
      .before = state,
      .after = state,
      .eventSeqId = targetSeqId,
  });
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

std::string CompletionTracker::commandBufferStatusName(MTLCommandBufferStatus status) const {
  switch (status) {
    case MTLCommandBufferStatusNotEnqueued:
      return "not-enqueued";
    case MTLCommandBufferStatusEnqueued:
      return "enqueued";
    case MTLCommandBufferStatusCommitted:
      return "committed";
    case MTLCommandBufferStatusScheduled:
      return "scheduled";
    case MTLCommandBufferStatusCompleted:
      return "completed";
    case MTLCommandBufferStatusError:
      return "error";
  }
  return "unknown";
}

bool CompletionTracker::inspect(id<MTLCommandBuffer> commandBuffer,
                                const CommandBufferDiagnostics& diagnostics,
                                const char* context) {
  if (!commandBuffer) {
    return false;
  }

  const MTLCommandBufferStatus status = [commandBuffer status];
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

  if (status == MTLCommandBufferStatusError) {
    std::ostringstream summary;
    summary << context << " seq=" << diagnostics.seqId << " status=error";
    if (NSError* error = [commandBuffer error]) {
      summary << " error=" << [[error localizedDescription] UTF8String];
    }
    lastErrorSummary_ = summary.str();
    dxmt9::util::logLine(dxmt9::util::LogLevel::Error, "dxmt9-metal", lastErrorSummary_);
  } else if (diagnostics.hasPresent) {
    lastErrorSummary_.clear();
  }

  if ([reinterpret_cast<id>(commandBuffer) respondsToSelector:@selector(logs)]) {
    const auto logsFn = reinterpret_cast<id (*)(id, SEL)>(objc_msgSend);
    NSArray* logs = logsFn(reinterpret_cast<id>(commandBuffer), @selector(logs));
    for (id logEntry in logs) {
      NSString* description = [logEntry description];
      if (!description) {
        continue;
      }
      dxmt9::util::logf(dxmt9::util::LogLevel::Warn, "dxmt9-metal",
                        "%s seq=%llu metal-log=%s",
                        context,
                        static_cast<unsigned long long>(diagnostics.seqId),
                        [description UTF8String]);
    }
  }

  return diagnostics.hasPresent || status == MTLCommandBufferStatusError;
}

}  // namespace dxmt9::core::metalqueue
