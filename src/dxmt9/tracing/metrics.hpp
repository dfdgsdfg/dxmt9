#pragma once

#include "../dxmt9_trace.hpp"
#include <string>

namespace dxmt9::tracing {
inline const char* metric_name(uint16_t metric) noexcept {
  switch (static_cast<Metric>(metric)) {
    case Metric::QueueReady: return "queue_ready";
    case Metric::QueueInflight: return "queue_inflight";
    case Metric::QueueCompleted: return "queue_completed_pending";
    case Metric::FrameDrawCalls: return "frame_draw_calls_delta";
    case Metric::FrameCommandBuffers: return "frame_command_buffers_delta";
    case Metric::FramePresentEncoded: return "frame_present_encoded_delta";
    case Metric::FrameEncodeChunkElapsedNs: return "frame_encode_chunk_elapsed_ns_delta";
    case Metric::FrameEncodeDrawElapsedNs: return "frame_encode_draw_elapsed_ns_delta";
    case Metric::NativeThreadCpuNs: return "native_thread_cpu_ns";
    case Metric::WaitMutexAcquireElapsedNs: return "wait_mutex_acquire_elapsed_ns";
    case Metric::WaitBodyElapsedNs: return "wait_body_elapsed_ns";
  }
  return "unknown";
}
inline const char* metric_unit(uint16_t metric) noexcept {
  return metric >= static_cast<uint16_t>(Metric::FrameEncodeChunkElapsedNs) ? "ns" : "count";
}
// Cold export only. Exact owner and thread identities prevent track aliasing.
inline std::string metric_track_name(const TraceEvent& event) {
  return std::string(metric_name(event.metric_id)) + "/owner/" +
      std::to_string(event.arg0) + "/thread/" + std::to_string(event.track_id);
}
}  // namespace dxmt9::tracing
