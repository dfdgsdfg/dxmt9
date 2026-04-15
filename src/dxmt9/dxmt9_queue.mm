#import <objc/message.h>

#include "dxmt9_queue.hpp"

#include "util/config/config.hpp"
#include "util/log/log.hpp"

#include <cstdio>
#include <cstdlib>
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
    case QueueSlotState::GPU:
      return "gpu";
  }
  return "unknown";
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
