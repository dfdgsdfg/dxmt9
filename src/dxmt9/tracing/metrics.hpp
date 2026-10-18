#pragma once

#include "../dxmt9_trace.hpp"
#include "../observations/metadata.hpp"
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
    case Metric::SnapshotCount: return "snapshot_count";
    case Metric::SnapshotElapsedNs: return "snapshot_elapsed_ns";
    case Metric::SnapshotIdentity: return "snapshot_identity";
    case Metric::DiagnosticValue: return "diagnostic_value";
  }
  return "unknown";
}
inline const char* metric_unit(uint16_t metric) noexcept {
  if (metric == static_cast<uint16_t>(Metric::SnapshotCount)) return "count";
  if (metric == static_cast<uint16_t>(Metric::SnapshotIdentity)) return "id";
  return metric >= static_cast<uint16_t>(Metric::FrameEncodeChunkElapsedNs) ? "ns" : "count";
}
inline std::string metric_unit(const TraceEvent& e) {
  if (e.metric_id == static_cast<uint16_t>(Metric::DiagnosticValue)) {
    const std::string semantics = e.diagnostic_payload;
    return semantics.substr(0, semantics.find(':'));
  }
  return metric_unit(e.metric_id);
}
// Cold export only. Exact owner and thread identities prevent track aliasing.
inline std::string metric_track_name(const TraceEvent& event) {
  const std::string name = event.observation_kind >= 7
      ? std::string(observations::source_name(event.observation_kind)) + "/" + event.diagnostic_schema
      : event.observation_kind >= 3
      ? std::string(observations::source_name(event.observation_kind)) + "/" +
          observations::field(event.observation_kind, event.observation_field).name
      : metric_name(event.metric_id);
  return name + "/owner/" +
      std::to_string(event.arg0) + "/thread/" + std::to_string(event.track_id);
}
}  // namespace dxmt9::tracing
