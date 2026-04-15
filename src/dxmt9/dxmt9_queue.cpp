#include "dxmt9_queue.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace dxmt9::core::metalqueue {

bool queueTraceEnabled() {
  static const bool enabled = [] {
    const char* env = std::getenv("DXMT_TRACE_QUEUE");
    return env != nullptr && env[0] != '\0' && std::strcmp(env, "0") != 0;
  }();
  return enabled;
}

const char* queueTraceFilePath() {
  static const char* path = std::getenv("DXMT_TRACE_FILE");
  return path && path[0] != '\0' ? path : nullptr;
}

u64 queueTraceFromSeq() {
  static const u64 value = [] {
    const char* env = std::getenv("DXMT_TRACE_QUEUE_FROM");
    if (!env || env[0] == '\0') {
      return 0ull;
    }
    return static_cast<u64>(std::strtoull(env, nullptr, 10));
  }();
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

}  // namespace dxmt9::core::metalqueue
