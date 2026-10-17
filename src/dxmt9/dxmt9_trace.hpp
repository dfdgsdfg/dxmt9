#pragma once

#include <atomic>
#include <cstdint>
#include <span>

namespace dxmt9::tracing {

// Event and track names are resolved on the control/export path. These numeric
// identifiers keep event storage POD and prevent borrowed strings crossing
// the observation boundary.
enum class Category : uint8_t {
  Import = 0,
  Replay,
  Queue,
  Encode,
  Wait,
  Submit,
  Completion,
  Pe,
  Gpu,
  Detail,
  Count,
};

enum class EventId : uint16_t {
  ImportBegin = 1,
  ImportEnd,
  ReplayBegin,
  ReplayEnd,
  QueuePublish,
  EncodeBegin,
  EncodeEnd,
  WaitBegin,
  WaitEnd,
  Submit,
  Completion,
  ReplayPublish,
  SubmitMember,
  CompletionDequeue,
  Retire,
  RawReady,
  DetailDraw,
  GpuCommandBufferBegin = 18,
  GpuCommandBufferEnd = 19,
  QueryPollBegin = 20,
  QueryPollEnd = 21,
  GpuStageBegin = 22,
  GpuStageEnd = 23,
  ReplayDispatchBegin = 24,
  ReplayDispatchEnd = 25,
  SubmitBegin = 26,
  PeChunkCommitBegin = 32,
  PeChunkCommitEnd = 33,
  PeClockCalibration = 34,
};

enum class Phase : uint8_t { Instant, Begin, End };

struct TraceEvent {
  uint64_t timestamp_ns;
  uint64_t track_id;
  uint64_t work_id;
  uint64_t scope_id;
  uint64_t source_record;
  uint64_t arg0;
  uint64_t arg1;
  // A second owner identity for typed cross-component bindings. Keep this
  // separate from arg1, whose meaning remains event-specific.
  uint64_t related_owner_id;
  uint64_t device_generation;
  uint64_t lane_id;
  uint64_t process_id;
  uint64_t thread_id;
  uint64_t guest_process_id;
  uint64_t guest_thread_id;
  uint64_t process_create_time100ns;
  uint64_t thread_create_time100ns;
  uint64_t clock_frequency;
  uint64_t uncertainty_ns;
  uint64_t source_timestamp;
  uint64_t source_clock_id;
  uint64_t calibration_id;
  double calibration_scale;
  double calibration_offset;
  uint64_t calibration_source_first;
  uint64_t calibration_host_first_ns;
  uint64_t calibration_source_second;
  uint64_t calibration_host_second_ns;
  uint64_t calibration_valid_begin;
  uint64_t calibration_valid_end;
  uint32_t clock_id;
  uint16_t event_id;
  uint8_t category;
  uint8_t phase;
};

struct TraceConfig {
  const char* request_path = nullptr;
  const char* receipt_path = nullptr;
  const char* raw_path = nullptr;
  const char* pftrace_path = nullptr;
};

void end_scope(uint64_t handle) noexcept;

struct TraceScope {
  uint64_t handle = 0;

  constexpr TraceScope() noexcept = default;
  TraceScope(uint8_t category, uint16_t begin_event, uint16_t end_event,
             uint64_t track_id, uint64_t work_id,
             uint64_t owner_id = 0, uint64_t related_owner_id = 0) noexcept;
  TraceScope(const TraceScope&) = delete;
  TraceScope& operator=(const TraceScope&) = delete;
  TraceScope(TraceScope&& other) noexcept;
  TraceScope& operator=(TraceScope&& other) noexcept;
  ~TraceScope() {
    if (handle)
      end_scope(handle);
  }
};

// This is the sole hot-path gate. It is false for every SDK-free or
// runtime-disabled build and becomes true only after a session is armed.
extern std::atomic<bool> g_trace_enabled;
extern std::atomic<bool> g_trace_detail_enabled;
extern std::atomic<uint64_t> g_trace_owner_registration_generation;
bool session_configured() noexcept;
inline bool enabled() noexcept {
  return g_trace_enabled.load(std::memory_order_relaxed);
}
inline bool enabled_detail() noexcept {
  return g_trace_detail_enabled.load(std::memory_order_relaxed);
}
uint64_t now_ns() noexcept;
uint64_t native_thread_id() noexcept;

// Control-path only. Reads and validates DXMT9_TRACE_CONFIG if paths are null,
// allocates all event/track storage, and starts process-lifetime collection.
bool prepare_and_start(const TraceConfig& config = {}) noexcept;
bool initialize_from_environment() noexcept;

// Cold integration point called once the Unix-side Metal device is available.
// The opaque handle is borrowed and is consumed only on the control path.
bool attach_metal_device(uintptr_t device_handle) noexcept;

// Emission consumes only POD values. No SDK call, lock, bridge operation or
// heap allocation occurs here. Callers should first test enabled().
bool emit(const TraceEvent& event) noexcept;
bool emit_batch(std::span<const TraceEvent> events,
                uint64_t loss_count = 0) noexcept;
// Capture this token at the start of a bounded external handler, then use the
// captured form when appending so a delayed old handler cannot target a rearm.
uint64_t capture_generation() noexcept;
bool emit_batch_captured(std::span<const TraceEvent> events,
                         uint64_t generation,
                         uint64_t loss_count = 0) noexcept;

// Control-path only. Closes admission, waits for admitted writers until the
// configured deadline, writes raw evidence and a native Perfetto trace, then
// atomically publishes the engine receipt. A false result means incomplete.
bool stop_and_finalize() noexcept;
bool stop_and_finalize(uint64_t expected_generation) noexcept;
void record_loss(uint32_t reason, uint64_t count = 1) noexcept;

}  // namespace dxmt9::tracing
