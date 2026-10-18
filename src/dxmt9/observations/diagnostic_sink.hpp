#pragma once

#include "metadata.hpp"
#include "../dxmt9_trace.hpp"
#include "util/diagnostic_report.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <string_view>
#include <cstdio>

namespace dxmt9::observations {
inline std::atomic<std::uint64_t> diagnostic_ordinal{0};
inline void diagnostic_loss(std::uint64_t generation) noexcept {
  tracing::TraceEvent invalid{};
  invalid.category = static_cast<std::uint8_t>(tracing::Category::Count);
  (void)tracing::emit_batch_captured({&invalid, 1}, generation, 1);
}
inline tracing::Category category(Source source) noexcept {
  switch (source) {
    case Source::QueryReport: case Source::WaitDetail: return tracing::Category::Wait;
    case Source::CpuDetail: return tracing::Category::Replay;
    case Source::PeReport: return tracing::Category::Pe;
    case Source::QueueReport: return tracing::Category::Queue;
    default: return tracing::Category::Encode;
  }
}
struct DiagnosticContext {
  std::uint64_t generation = tracing::enabled() ? tracing::capture_generation() : 0;
  std::uint64_t timestamp = generation ? tracing::now_ns() : 0;
  std::uint64_t record = generation ? ++diagnostic_ordinal : 0;
  std::uint64_t work = 0;
};
inline tracing::TraceEvent diagnostic_event(Source source, const DiagnosticContext& c) noexcept {
  tracing::TraceEvent e{};
  e.timestamp_ns = c.timestamp;
  e.observation_record = c.record;
  e.observation_kind = static_cast<std::uint16_t>(source);
  e.work_id = c.work;
  e.category = static_cast<std::uint8_t>(category(source));
  e.event_id = static_cast<std::uint16_t>(tracing::EventId::Diagnostic);
  e.phase = static_cast<std::uint8_t>(tracing::Phase::Instant);
  return e;
}
// The native report is admitted atomically. Compatibility output is independent.
inline bool report(Source source, std::string_view schema, std::string_view data,
                   const DiagnosticContext& c = {}, bool truncated = false,
                   const tracing::TraceEvent* identity = nullptr, bool allowFiltered = false) noexcept {
  if (!c.generation) return false;
  constexpr std::size_t payload = 191, capacity = 64;
  const auto count = std::max<std::size_t>(1, (data.size() + payload - 1) / payload);
  if (truncated || schema.empty() || schema.size() >= 128 || data.size() > payload * capacity ||
      data.find('\0') != data.npos || count > capacity) {
    diagnostic_loss(c.generation);
    return false;
  }
  std::array<tracing::TraceEvent, capacity> events;
  for (std::size_t i = 0; i < count; ++i) {
    auto& e = events[i];
    e = diagnostic_event(source, c);
    if (identity) {
      e.guest_process_id = identity->guest_process_id;
      e.guest_thread_id = identity->guest_thread_id;
      e.process_create_time100ns = identity->process_create_time100ns;
      e.thread_create_time100ns = identity->thread_create_time100ns;
    }
    std::memcpy(e.diagnostic_schema, schema.data(), schema.size());
    const auto offset = i * payload;
    const auto size = std::min(payload, data.size() - offset);
    if (size) std::memcpy(e.diagnostic_payload, data.data() + offset, size);
    e.diagnostic_part = static_cast<std::uint16_t>(i);
    e.diagnostic_parts = static_cast<std::uint16_t>(count);
    e.observation_mode = 1; // Exact acquired report bytes, source-defined units.
  }
  return allowFiltered ? tracing::emit_batch_captured({events.data(), count}, c.generation)
      : tracing::emit_observations({events.data(), count}, c.generation);
}
inline bool value(Source source, std::string_view key, std::uint64_t value,
                  const char* semantics, const DiagnosticContext& c = {}) noexcept {
  if (!c.generation) return false;
  if (key.size() >= 128 || std::strlen(semantics) >= 192) {
    diagnostic_loss(c.generation); return false;
  }
  auto e = diagnostic_event(source, c);
  std::memcpy(e.diagnostic_schema, key.data(), key.size());
  std::strcpy(e.diagnostic_payload, semantics);
  e.metric_value = value;
  e.observation_mode = 2;
  if (value <= INT64_MAX && std::strncmp(semantics, "id:", 3) != 0) {
    e.phase = static_cast<std::uint8_t>(tracing::Phase::Counter);
    e.metric_id = static_cast<std::uint16_t>(tracing::Metric::DiagnosticValue);
  }
  e.diagnostic_parts = 1;
  return tracing::emit_observation(e, c.generation);
}
inline void consume_report(const char* tag, std::string_view data, bool truncated) noexcept {
  if (!tracing::enabled()) return;
  const std::string_view name(tag);
  Source source;
  if (name.find("query") != name.npos) source = Source::QueryReport;
  else if (name.find("pe-") != name.npos) source = Source::PeReport;
  else if (name.find("perf") != name.npos) source = Source::PerfReport;
  else if (name.find("queue") != name.npos) source = Source::QueueReport;
  else if (name.starts_with("dxmt9") || name.starts_with("dxmt")) source = Source::Renderer;
  else return;
  (void)report(source, name, data, {}, truncated);
}
inline void install_diagnostic_consumer() noexcept {
  diagnostics::consumer.store(consume_report, std::memory_order_release);
}
}  // namespace dxmt9::observations
