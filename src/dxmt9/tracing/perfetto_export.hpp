#pragma once

#include "../dxmt9_trace.hpp"

#include <span>

namespace dxmt9::tracing::detail {

struct ExportResult {
  bool success = false;
  bool flush_complete = false;
  uint64_t lost_packets = 0;
  uint64_t overwritten_bytes = 0;
  uint64_t discarded_chunks = 0;
};

ExportResult export_pftrace(std::span<const TraceEvent> events,
                            const char* path, const char* sdk_revision,
                            uint32_t max_bytes,
                            uint32_t flush_timeout_ms) noexcept;

}  // namespace dxmt9::tracing::detail
