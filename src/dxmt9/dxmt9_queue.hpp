#pragma once

#include <cstdint>
#include <string>

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

bool queueTraceEnabled();
const char* queueTraceFilePath();
u64 queueTraceFromSeq();

void emitQueueTraceLine(const std::string& line);
void emitTextureTraceLine(const std::string& line);

}  // namespace dxmt9::core::metalqueue
