#import <objc/message.h>

#include "dxmt9_queue.hpp"

#include "util/util_env.hpp"
#include "util/util_log.hpp"

#include <cstdio>
#include <cstdlib>
#include <sstream>

namespace dxmt9::core::metalqueue {

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
