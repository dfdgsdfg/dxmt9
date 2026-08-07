#include "dxmt9_queue.hpp"

#include "util/config/config.hpp"

#include <cstdio>
#include <sstream>

namespace dxmt9::core::metalqueue {

namespace {

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
    case QueueSlotState::Retiring:
      return "retiring";
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

QueueTraceSnapshot makeQueueTraceSnapshot(std::optional<size_t> slotIndex,
                                          u64 eventSeqId,
                                          std::optional<size_t> writingSlot,
                                          size_t writeIndex,
                                          size_t readyCount,
                                          size_t completedQueueCount,
                                          size_t inflightCount,
                                          u64 completedSeqId,
                                          u64 lastCommittedSeqId,
                                          std::span<const ChunkSlotControl> slots) {
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
        .commandCount = slot.commandCount(),
    });
  }
  return makeQueueTraceSnapshot(traceState);
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
                         std::span<const ChunkSlotControl> slots,
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
                          std::span<const ChunkSlotControl> slots,
                          const char* extra) {
  traceQueueEvent(event, makeQueueTraceSnapshot(slotIndex, eventSeqId, writingSlot, writeIndex, readyCount,
                                                completedQueueCount, inflightCount, completedSeqId,
                                                lastCommittedSeqId, slots),
                  extra);
}

}  // namespace dxmt9::core::metalqueue
