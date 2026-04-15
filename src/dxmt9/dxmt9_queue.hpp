#pragma once

#import <Metal/Metal.h>

#include <cstdint>
#include <optional>
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

class ChunkSummaryBuilder {
 public:
  ChunkSummaryBuilder(u64 seqId, size_t slotIndex);

  void observeDraw(u32 compatFlags);
  void observeBlit();
  void observePresent(u32 compatFlags);

  CommandBufferDiagnostics finish() const;

 private:
  CommandBufferDiagnostics diagnostics_{};
};

class QueueTraceSnapshotBuilder {
 public:
  QueueTraceSnapshotBuilder(std::optional<size_t> slotIndex, u64 eventSeqId);

  void setWritingSlot(std::optional<size_t> slotIndex);
  void setWriteIndex(size_t index);
  void setReadyCount(size_t count);
  void setCompletedQueueCount(size_t count);
  void setInflightCount(size_t count);
  void setCompletedSeqId(u64 seqId);
  void setLastCommittedSeqId(u64 seqId);
  void addActiveSlot(size_t index, QueueSlotState state, u64 seqId, size_t commandCount);

  QueueTraceSnapshot finish() &&;

 private:
  QueueTraceSnapshot snapshot_{};
};

template <typename EnumerateFn>
CommandBufferDiagnostics makeChunkSummary(u64 seqId, size_t slotIndex, EnumerateFn&& enumerate) {
  ChunkSummaryBuilder builder(seqId, slotIndex);
  enumerate(builder);
  return builder.finish();
}

template <typename EnumerateFn>
QueueTraceSnapshot makeQueueTraceSnapshot(std::optional<size_t> slotIndex,
                                         std::optional<size_t> writingSlot,
                                         size_t writeIndex,
                                         size_t readyCount,
                                         size_t completedQueueCount,
                                         size_t inflightCount,
                                         u64 completedSeqId,
                                         u64 lastCommittedSeqId,
                                         u64 eventSeqId,
                                         EnumerateFn&& enumerate) {
  QueueTraceSnapshotBuilder builder(slotIndex, eventSeqId);
  builder.setWritingSlot(writingSlot);
  builder.setWriteIndex(writeIndex);
  builder.setReadyCount(readyCount);
  builder.setCompletedQueueCount(completedQueueCount);
  builder.setInflightCount(inflightCount);
  builder.setCompletedSeqId(completedSeqId);
  builder.setLastCommittedSeqId(lastCommittedSeqId);
  enumerate(builder);
  return std::move(builder).finish();
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

class CompletionTracker {
 public:
  bool inspect(id<MTLCommandBuffer> commandBuffer, const CommandBufferDiagnostics& diagnostics, const char* context);
  const std::string& lastErrorSummary() const noexcept { return lastErrorSummary_; }

 private:
  std::string commandBufferStatusName(MTLCommandBufferStatus status) const;

  std::string lastErrorSummary_;
};

}  // namespace dxmt9::core::metalqueue
