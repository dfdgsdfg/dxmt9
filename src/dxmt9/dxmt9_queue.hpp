#pragma once

#import <Metal/Metal.h>

#include "dxmt9_backend_types.hpp"

#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <vector>

namespace dxmt9::core::metalqueue {

using u32 = std::uint32_t;
using u64 = std::uint64_t;

struct CommandBufferDiagnostics {
  u64 seqId = 0;
  size_t slotIndex = 0;
  bool hasDraw = false;
  bool hasPresent = false;
  bool hasBlit = false;
  u32 frame = 0;
  u32 compatFlags = 0;
};

enum class QueueSlotState {
  Free = 0,
  Writing = 1,
  Pending = 2,
  Encoding = 3,
  GPU = 4,
};

struct ActiveSlotInfo {
  size_t index = 0;
  QueueSlotState state = QueueSlotState::Free;
  u64 seqId = 0;
  size_t commandCount = 0;
};

struct QueueTraceSnapshot {
  std::optional<size_t> slotIndex;
  std::optional<size_t> writingSlot;
  size_t writeIndex = 0;
  size_t readyCount = 0;
  size_t completedQueueCount = 0;
  size_t inflightCount = 0;
  u64 completedSeqId = 0;
  u64 lastCommittedSeqId = 0;
  u64 eventSeqId = 0;
  std::vector<ActiveSlotInfo> activeSlots;
};

struct ChunkSummaryInput {
  u64 seqId = 0;
  size_t slotIndex = 0;
  bool hasDraw = false;
  bool hasPresent = false;
  bool hasBlit = false;
  u32 frame = 0;
  u32 compatFlags = 0;
};

enum class ChunkObservationKind {
  Draw,
  Blit,
  Present,
};

struct ChunkObservation {
  ChunkObservationKind kind = ChunkObservationKind::Draw;
  u32 compatFlags = 0;
};

struct QueueTraceState {
  std::optional<size_t> slotIndex;
  std::optional<size_t> writingSlot;
  size_t writeIndex = 0;
  size_t readyCount = 0;
  size_t completedQueueCount = 0;
  size_t inflightCount = 0;
  u64 completedSeqId = 0;
  u64 lastCommittedSeqId = 0;
  u64 eventSeqId = 0;
  std::vector<ActiveSlotInfo> activeSlots;
};

struct QueueLifecycleContext {
  std::optional<size_t> writingSlot;
  size_t writeIndex = 0;
  size_t readyCount = 0;
  size_t completedQueueCount = 0;
  size_t inflightCount = 0;
  u64 completedSeqId = 0;
  u64 lastCommittedSeqId = 0;
};

enum class QueueLifecycleEvent {
  PresentEnqueue,
  WriterWaitBegin,
  WriterWaitEnd,
  WriterAcquire,
  CommitEmpty,
  CommitWaitBegin,
  CommitWaitEnd,
  CommitPublish,
  EncodeDequeue,
  EncodeCommit,
  GpuComplete,
  FinishInline,
  FinishDequeue,
  ReclaimFree,
  WaitSeqBegin,
  WaitSeqEnd,
};

CommandBufferDiagnostics summarizeChunk(u64 seqId,
                                        size_t slotIndex,
                                        std::span<const ChunkObservation> observations);
CommandBufferDiagnostics summarizeCommands(u64 seqId,
                                          size_t slotIndex,
                                          std::span<const MetalCommandRecord> commands,
                                          const std::function<u32(Handle)>& resolveSurfaceFlags);
QueueTraceSnapshot makeQueueTraceSnapshot(const QueueTraceState& state);

template <typename CommandContainer, typename ObservationMapper>
CommandBufferDiagnostics summarizeChunk(u64 seqId,
                                        size_t slotIndex,
                                        const CommandContainer& commands,
                                        ObservationMapper&& mapObservation) {
  std::vector<ChunkObservation> observations;
  observations.reserve(commands.size());
  for (const auto& command : commands) {
    observations.push_back(std::invoke(std::forward<ObservationMapper>(mapObservation), command));
  }
  return summarizeChunk(seqId, slotIndex, std::span<const ChunkObservation>(observations.data(), observations.size()));
}

template <typename SlotContainer, typename SlotMapper>
QueueTraceSnapshot makeQueueTraceSnapshot(std::optional<size_t> slotIndex,
                                          std::optional<size_t> writingSlot,
                                          size_t writeIndex,
                                          size_t readyCount,
                                          size_t completedQueueCount,
                                          size_t inflightCount,
                                          u64 completedSeqId,
                                          u64 lastCommittedSeqId,
                                          u64 eventSeqId,
                                          const SlotContainer& slots,
                                          SlotMapper&& mapSlot) {
  QueueTraceState state;
  state.slotIndex = slotIndex;
  state.writingSlot = writingSlot;
  state.writeIndex = writeIndex;
  state.readyCount = readyCount;
  state.completedQueueCount = completedQueueCount;
  state.inflightCount = inflightCount;
  state.completedSeqId = completedSeqId;
  state.lastCommittedSeqId = lastCommittedSeqId;
  state.eventSeqId = eventSeqId;
  state.activeSlots.reserve(slots.size());
  for (size_t i = 0; i < slots.size(); ++i) {
    auto mapped = std::invoke(std::forward<SlotMapper>(mapSlot), i, slots[i]);
    if (mapped.has_value()) {
      state.activeSlots.push_back(*mapped);
    }
  }
  return makeQueueTraceSnapshot(state);
}

template <typename SlotContainer>
QueueTraceSnapshot makeQueueTraceSnapshot(std::optional<size_t> slotIndex,
                                          std::optional<size_t> writingSlot,
                                          size_t writeIndex,
                                          size_t readyCount,
                                          size_t completedQueueCount,
                                          size_t inflightCount,
                                          u64 completedSeqId,
                                          u64 lastCommittedSeqId,
                                          u64 eventSeqId,
                                          const SlotContainer& slots) {
  QueueTraceState state;
  state.slotIndex = slotIndex;
  state.writingSlot = writingSlot;
  state.writeIndex = writeIndex;
  state.readyCount = readyCount;
  state.completedQueueCount = completedQueueCount;
  state.inflightCount = inflightCount;
  state.completedSeqId = completedSeqId;
  state.lastCommittedSeqId = lastCommittedSeqId;
  state.eventSeqId = eventSeqId;
  state.activeSlots.reserve(slots.size());
  for (size_t i = 0; i < slots.size(); ++i) {
    const auto& slot = slots[i];
    if (slot.state == ChunkSlot::State::Free) {
      continue;
    }
    state.activeSlots.push_back(ActiveSlotInfo{
        .index = i,
        .state = static_cast<QueueSlotState>(static_cast<int>(slot.state)),
        .seqId = slot.seqId,
        .commandCount = slot.commands.size(),
    });
  }
  return makeQueueTraceSnapshot(state);
}

template <typename SlotContainer, typename SlotMapper>
void traceQueueEvent(const char* event,
                     std::optional<size_t> slotIndex,
                     std::optional<size_t> writingSlot,
                     size_t writeIndex,
                     size_t readyCount,
                     size_t completedQueueCount,
                     size_t inflightCount,
                     u64 completedSeqId,
                     u64 lastCommittedSeqId,
                     u64 eventSeqId,
                     const SlotContainer& slots,
                     SlotMapper&& mapSlot,
                     const char* extra = nullptr) {
  traceQueueEvent(
      event,
      makeQueueTraceSnapshot(slotIndex, writingSlot, writeIndex, readyCount, completedQueueCount,
                             inflightCount, completedSeqId, lastCommittedSeqId, eventSeqId, slots,
                             std::forward<SlotMapper>(mapSlot)),
      extra);
}

template <typename SlotContainer>
void traceQueueEvent(const char* event,
                     std::optional<size_t> slotIndex,
                     std::optional<size_t> writingSlot,
                     size_t writeIndex,
                     size_t readyCount,
                     size_t completedQueueCount,
                     size_t inflightCount,
                     u64 completedSeqId,
                     u64 lastCommittedSeqId,
                     u64 eventSeqId,
                     const SlotContainer& slots,
                     const char* extra = nullptr) {
  traceQueueEvent(
      event,
      makeQueueTraceSnapshot(slotIndex, writingSlot, writeIndex, readyCount, completedQueueCount,
                             inflightCount, completedSeqId, lastCommittedSeqId, eventSeqId, slots),
      extra);
}

bool queueTraceEnabled();
const char* queueTraceFilePath();
u64 queueTraceFromSeq();

void emitQueueTraceLine(const std::string& line);
void emitTextureTraceLine(const std::string& line);

CommandBufferDiagnostics summarizeChunk(const ChunkSummaryInput& input);
bool shouldTraceQueue(const QueueTraceSnapshot& snapshot);
std::string formatActiveSlots(const QueueTraceSnapshot& snapshot);
void traceQueueEvent(const char* event, const QueueTraceSnapshot& snapshot, const char* extra = nullptr);
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
                         const char* extra = nullptr);
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
                          const char* extra = nullptr);

class QueueLifecycleController {
 public:
  CommandBufferDiagnostics summarizeSubmission(
      u64 seqId,
      size_t slotIndex,
      std::span<const MetalCommandRecord> commands,
      const std::function<u32(Handle)>& resolveSurfaceFlags) const;

  template <typename ReadyContainer, typename CompletedContainer, typename SlotContainer>
  void onEvent(QueueLifecycleEvent event,
               std::optional<size_t> slotIndex,
               u64 eventSeqId,
               std::optional<size_t> writingSlot,
               size_t writeIndex,
               const ReadyContainer& readySlots,
               const CompletedContainer& completedSeqQueue,
               size_t inflightCount,
               u64 completedSeqId,
               u64 lastCommittedSeqId,
               const SlotContainer& slots,
               const char* extra = nullptr) const {
    const QueueLifecycleContext context{
        .writingSlot = writingSlot,
        .writeIndex = writeIndex,
        .readyCount = readySlots.size(),
        .completedQueueCount = completedSeqQueue.size(),
        .inflightCount = inflightCount,
        .completedSeqId = completedSeqId,
        .lastCommittedSeqId = lastCommittedSeqId,
    };
    traceLifecycleEvent(event, slotIndex, eventSeqId, context.writingSlot, context.writeIndex,
                        context.readyCount, context.completedQueueCount, context.inflightCount,
                        context.completedSeqId, context.lastCommittedSeqId,
                        std::span<const ChunkSlot>(slots.data(), slots.size()), extra);
  }

  template <typename ReadyContainer, typename CompletedContainer, typename SlotContainer>
  void onPresentEnqueue(size_t slotIndex,
                        u64 eventSeqId,
                        std::optional<size_t> writingSlot,
                        size_t writeIndex,
                        const ReadyContainer& readySlots,
                        const CompletedContainer& completedSeqQueue,
                        size_t inflightCount,
                        u64 completedSeqId,
                        u64 lastCommittedSeqId,
                        const SlotContainer& slots,
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
    onEvent(QueueLifecycleEvent::PresentEnqueue, slotIndex, eventSeqId, writingSlot, writeIndex,
            readySlots, completedSeqQueue, inflightCount, completedSeqId, lastCommittedSeqId,
            slots);
  }
};

class CompletionTracker {
 public:
  bool inspect(id<MTLCommandBuffer> commandBuffer, const CommandBufferDiagnostics& diagnostics, const char* context);
  const std::string& lastErrorSummary() const noexcept { return lastErrorSummary_; }

 private:
  std::string commandBufferStatusName(MTLCommandBufferStatus status) const;

  std::string lastErrorSummary_;
};

}  // namespace dxmt9::core::metalqueue
