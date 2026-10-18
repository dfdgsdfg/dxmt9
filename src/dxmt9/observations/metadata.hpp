#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <span>

namespace dxmt9::observations {
// Cold-side schema expansion. Numeric fields are copied at acquisition; names
// and decimal display are resolved only by raw/native exporters.
inline std::span<const char* const> typed_fields(std::string_view schema, uint16_t encoding) {
  static constexpr const char* endpoint[] = {"source", "before", "after", "resolution"};
  static constexpr const char* cpu[] = {"wall_ns", "cpu_ns", "tid", "owner", "id", "value",
      "frame", "event", "qualified", "native_before_ns", "native_after_ns", "resolution_ns"};
  static constexpr const char* wait[] = {"owner", "target", "query", "queue", "sequence",
      "initializer", "requested_queue", "begin", "acquired", "end", "tid", "first_frame",
      "last_frame", "waits", "site", "before_raw", "before_queued", "before_completed",
      "before_state", "after_raw", "after_queued", "after_completed", "after_state", "qualified",
      "begin_native_before", "begin_native_after", "acquired_native_before", "acquired_native_after",
      "end_native_before", "end_native_after", "resolution_ns"};
  static constexpr const char* pe[] = {"record_version", "sample_ordinal", "target_process_id",
      "target_thread_id", "target_process_create_100ns", "target_thread_create_100ns", "qpc_frequency",
      "qpc_before_suspend", "qpc_after_resume", "pc", "outcome", "population_attempts",
      "population_successes", "suspend_failures", "context_failures", "resume_failures", "omitted_samples"};
  if (encoding == 5) return endpoint;
  if (encoding != 4) return {};
  if (schema == "cpu-supply-row-v2") return cpu;
  if (schema == "wait-row-v2") return wait;
  if (schema == "pe-pc-sample-v1" || schema == "pe-pc-population-v1") return pe;
  return {};
}
template<class Event, class Output>
inline void expand_typed_fields(const Event& event, Output output) {
  const auto fields = typed_fields(event.diagnostic_schema, event.observation_mode);
  if (event.diagnostic_size > sizeof(event.diagnostic_payload) || event.diagnostic_size % 8) return;
  const size_t first = event.diagnostic_part * (sizeof(event.diagnostic_payload) / 8);
  for (size_t offset = 0; offset < event.diagnostic_size / 8 && first + offset < fields.size(); ++offset) {
    uint64_t value = 0;
    for (size_t byte = 0; byte < 8; ++byte)
      value |= uint64_t(static_cast<unsigned char>(event.diagnostic_payload[offset * 8 + byte])) << (byte * 8);
    output(std::string("diagnostic_field.") + fields[first + offset], std::to_string(value));
  }
}
inline std::string diagnostic_hex(std::string_view bytes) {
  constexpr char hex[] = "0123456789abcdef";
  std::string result;
  result.reserve(bytes.size() * 2);
  for (unsigned char ch : bytes) {
    result.push_back(hex[ch >> 4]); result.push_back(hex[ch & 15]);
  }
  return result;
}
inline std::string diagnostic_display(std::string_view bytes) {
  std::string result;
  for (unsigned char ch : bytes) {
    if (ch >= 32 && ch < 127) result.push_back(static_cast<char>(ch));
    else { result += "\\x"; result += diagnostic_hex({reinterpret_cast<const char*>(&ch), 1}); }
  }
  return result;
}
enum class Source : std::uint16_t {
  CpuSupply = 1, Wait = 2, QueryBackend = 3, QueryReceipt = 4,
  PerfFrame = 5, CompatAppend = 6,
  QueryReport = 7, PerfReport = 8, ProcessMemory = 9,
  CpuDetail = 10, WaitDetail = 11, PeReport = 12,
  QueueReport = 13, Signpost = 14, Renderer = 15,
};
inline const char* source_name(std::uint16_t source) noexcept {
  switch (static_cast<Source>(source)) {
    case Source::CpuSupply: return "cpu-supply";
    case Source::Wait: return "wait";
    case Source::QueryBackend: return "query-backend";
    case Source::QueryReceipt: return "query-receipt";
    case Source::PerfFrame: return "perf-frame";
    case Source::CompatAppend: return "compat-append";
    case Source::QueryReport: return "query-report";
    case Source::PerfReport: return "perf-report";
    case Source::ProcessMemory: return "process-memory";
    case Source::CpuDetail: return "cpu-detail";
    case Source::WaitDetail: return "wait-detail";
    case Source::PeReport: return "pe-report";
    case Source::QueueReport: return "queue-report";
    case Source::Signpost: return "signpost";
    case Source::Renderer: return "renderer";
  }
  return "unknown";
}
inline const char* timestamp_kind(std::uint16_t source, std::uint16_t mode = 0) noexcept {
  if (source == 10 || source == 11) return (mode == 3 || mode == 5) ? "bracketed-endpoint" : "detailed-record-observed";
  if (source == 12) return "report-received";
  if (source >= 7) return "report-observed";
  return source == 1 ? "cpu-query-start" : source == 2
      ? "wait-result-observed" : "snapshot-observed";
}
struct Field { const char* name; const char* unit; const char* aggregation; };
inline constexpr Field backend_fields[] = {
  {"calls", "count", "cumulative"},
  {"flush", "count", "cumulative"},
  {"samples", "count", "cumulative"},
  {"invalid_clocks", "count", "cumulative"},
  {"markers", "count", "cumulative"},
  {"covered_pending", "count", "cumulative"},
  {"request_none", "count", "cumulative"},
  {"request_rejected", "count", "cumulative"},
  {"request_existing_target", "count", "cumulative"},
  {"request_submitted", "count", "cumulative"},
  {"request_new", "count", "cumulative"},
  {"result_invalid", "count", "cumulative"},
  {"result_failed", "count", "cumulative"},
  {"result_pending", "count", "cumulative"},
  {"result_complete", "count", "cumulative"},
  {"queue_wait_elapsed_ns", "ns", "cumulative"},
  {"classify_elapsed_ns", "ns", "cumulative"},
  {"marker_elapsed_ns", "ns", "cumulative"},
  {"request_setup_elapsed_ns", "ns", "cumulative"},
  {"pending_wait_elapsed_ns", "ns", "cumulative"},
  {"decision_elapsed_ns", "ns", "cumulative"},
  {"wake_elapsed_ns", "ns", "cumulative"},
  {"exit_elapsed_ns", "ns", "cumulative"},
  {"queue_wait_max_elapsed_ns", "ns", "maximum"},
  {"classify_max_elapsed_ns", "ns", "maximum"},
  {"marker_max_elapsed_ns", "ns", "maximum"},
  {"request_setup_max_elapsed_ns", "ns", "maximum"},
  {"pending_wait_max_elapsed_ns", "ns", "maximum"},
  {"decision_max_elapsed_ns", "ns", "maximum"},
  {"wake_max_elapsed_ns", "ns", "maximum"},
  {"exit_max_elapsed_ns", "ns", "maximum"},
};
inline constexpr Field receipt_fields[] = {
  {"calls", "count", "cumulative"},
  {"flush", "count", "cumulative"},
  {"samples", "count", "cumulative"},
  {"invalid_clocks", "count", "cumulative"},
  {"route_missing_target", "count", "cumulative"},
  {"route_non_raw", "count", "cumulative"},
  {"route_replay_failed", "count", "cumulative"},
  {"route_superseded", "count", "cumulative"},
  {"route_not_ready", "count", "cumulative"},
  {"route_raw_mismatch", "count", "cumulative"},
  {"route_ineligible", "count", "cumulative"},
  {"route_identity_mismatch", "count", "cumulative"},
  {"route_handled", "count", "cumulative"},
  {"kind_0", "count", "cumulative"},
  {"kind_1", "count", "cumulative"},
  {"kind_2", "count", "cumulative"},
  {"kind_3", "count", "cumulative"},
  {"kind_4", "count", "cumulative"},
  {"kind_5", "count", "cumulative"},
  {"result_ok", "count", "cumulative"},
  {"result_pending", "count", "cumulative"},
  {"result_error", "count", "cumulative"},
  {"result_fallback", "count", "cumulative"},
  {"sampled_result_ok", "count", "cumulative"},
  {"sampled_result_pending", "count", "cumulative"},
  {"sampled_result_error", "count", "cumulative"},
  {"sampled_result_fallback", "count", "cumulative"},
  {"capture_elapsed_ns", "ns", "cumulative"},
  {"replay_wait_elapsed_ns", "ns", "cumulative"},
  {"snapshot_elapsed_ns", "ns", "cumulative"},
  {"backend_elapsed_ns", "ns", "cumulative"},
  {"final_elapsed_ns", "ns", "cumulative"},
  {"ok_capture_elapsed_ns", "ns", "cumulative"},
  {"ok_replay_wait_elapsed_ns", "ns", "cumulative"},
  {"ok_snapshot_elapsed_ns", "ns", "cumulative"},
  {"ok_backend_elapsed_ns", "ns", "cumulative"},
  {"ok_final_elapsed_ns", "ns", "cumulative"},
  {"pending_capture_elapsed_ns", "ns", "cumulative"},
  {"pending_replay_wait_elapsed_ns", "ns", "cumulative"},
  {"pending_snapshot_elapsed_ns", "ns", "cumulative"},
  {"pending_backend_elapsed_ns", "ns", "cumulative"},
  {"pending_final_elapsed_ns", "ns", "cumulative"},
  {"error_capture_elapsed_ns", "ns", "cumulative"},
  {"error_replay_wait_elapsed_ns", "ns", "cumulative"},
  {"error_snapshot_elapsed_ns", "ns", "cumulative"},
  {"error_backend_elapsed_ns", "ns", "cumulative"},
  {"error_final_elapsed_ns", "ns", "cumulative"},
  {"fallback_capture_elapsed_ns", "ns", "cumulative"},
  {"fallback_replay_wait_elapsed_ns", "ns", "cumulative"},
  {"fallback_snapshot_elapsed_ns", "ns", "cumulative"},
  {"fallback_backend_elapsed_ns", "ns", "cumulative"},
  {"fallback_final_elapsed_ns", "ns", "cumulative"},
};
inline constexpr Field append_fields[] = {
  {"batch_elapsed_ns", "ns", "cumulative"},
  {"batch_calls", "count", "cumulative"},
  {"batch_reserve_elapsed_ns", "ns", "cumulative"},
  {"batch_reserve_calls", "count", "cumulative"},
  {"append_elapsed_ns", "ns", "cumulative"},
  {"append_calls", "count", "cumulative"},
  {"preflight_elapsed_ns", "ns", "cumulative"},
  {"preflight_calls", "count", "cumulative"},
  {"uniform_lookup_elapsed_ns", "ns", "cumulative"},
  {"uniform_lookup_calls", "count", "cumulative"},
  {"local_reserve_elapsed_ns", "ns", "cumulative"},
  {"local_reserve_calls", "count", "cumulative"},
  {"uniform_append_elapsed_ns", "ns", "cumulative"},
  {"uniform_append_calls", "count", "cumulative"},
  {"state_elapsed_ns", "ns", "cumulative"},
  {"state_calls", "count", "cumulative"},
  {"soa_elapsed_ns", "ns", "cumulative"},
  {"soa_calls", "count", "cumulative"},
};
inline constexpr Field perf_fields[] = {
#define X(member, name, unit, aggregation) {name, unit, aggregation},
#include "perf_fields.inc"
#undef X
  {"regressed_fields", "count", "snapshot"},
};
inline std::size_t field_count(std::uint16_t source) noexcept {
  switch (static_cast<Source>(source)) {
    case Source::QueryBackend: return std::size(backend_fields);
    case Source::QueryReceipt: return std::size(receipt_fields);
    case Source::PerfFrame: return std::size(perf_fields);
    case Source::CompatAppend: return std::size(append_fields);
    default: return 0;
  }
}
inline constexpr std::size_t max_snapshot_fields = std::size(perf_fields);
static_assert(std::size(backend_fields) <= max_snapshot_fields &&
              std::size(receipt_fields) <= max_snapshot_fields &&
              std::size(append_fields) <= max_snapshot_fields);
inline Field field(std::uint16_t source, std::uint16_t index) noexcept {
  switch (static_cast<Source>(source)) {
    case Source::QueryBackend:
      if (index < std::size(backend_fields)) return backend_fields[index];
      break;
    case Source::QueryReceipt:
      if (index < std::size(receipt_fields)) return receipt_fields[index];
      break;
    case Source::PerfFrame:
      if (index < std::size(perf_fields)) return perf_fields[index];
      break;
    case Source::CompatAppend:
      if (index < std::size(append_fields)) return append_fields[index];
      break;
    default: break;
  }
  return {"unknown", "unknown", "unknown"};
}
}  // namespace dxmt9::observations
