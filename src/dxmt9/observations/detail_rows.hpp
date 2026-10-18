#pragma once

#include "diagnostic_sink.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>

namespace dxmt9::observations {

// Schema field order is the wire contract for observation_mode 4. Keep names
// here so exporters and tests can decode the bounded little-endian values.
inline constexpr std::array<std::string_view, 12> cpu_supply_row_fields = {
    "wall_ns", "cpu_ns", "tid", "owner", "id", "value", "frame",
    "event", "qualified", "native_before_ns", "native_after_ns",
    "resolution_ns"};
inline constexpr std::array<std::string_view, 31> wait_row_fields = {
    "owner", "target", "query", "queue", "sequence", "initializer",
    "requested_queue", "begin", "acquired", "end", "tid", "first_frame",
    "last_frame", "waits", "site", "before_raw", "before_queued",
    "before_completed", "before_state", "after_raw", "after_queued",
    "after_completed", "after_state", "qualified", "begin_native_before",
    "begin_native_after", "acquired_native_before", "acquired_native_after",
    "end_native_before", "end_native_after", "resolution_ns"};

inline constexpr std::size_t detail_payload_capacity = 192;

template <std::size_t FieldCount>
inline constexpr std::size_t detail_event_count =
    (FieldCount * sizeof(std::uint64_t) + detail_payload_capacity - 1) /
    detail_payload_capacity;

template <std::size_t FieldCount>
using DetailEvents = std::array<tracing::TraceEvent, detail_event_count<FieldCount>>;

template <std::size_t FieldCount>
inline DetailEvents<FieldCount> make_detail_events(
    Source source, std::string_view schema, const DiagnosticContext& context,
    const std::array<std::uint64_t, FieldCount>& fields) noexcept {
  static_assert(detail_event_count<FieldCount> <= 3);
  DetailEvents<FieldCount> events{};
  constexpr std::size_t byteCount = FieldCount * sizeof(std::uint64_t);
  constexpr std::size_t eventCount = detail_event_count<FieldCount>;
  for (std::size_t part = 0; part < eventCount; ++part) {
    auto& event = events[part];
    event = diagnostic_event(source, context);
    const auto schemaSize = schema.size() < sizeof(event.diagnostic_schema)
        ? schema.size() : sizeof(event.diagnostic_schema) - 1;
    if (schemaSize) std::memcpy(event.diagnostic_schema, schema.data(), schemaSize);
    const auto offset = part * detail_payload_capacity;
    const auto size = byteCount - offset < detail_payload_capacity
        ? byteCount - offset : detail_payload_capacity;
    for (std::size_t byte = 0; byte < size; ++byte) {
      const auto fieldIndex = (offset + byte) / sizeof(std::uint64_t);
      const auto fieldByte = (offset + byte) % sizeof(std::uint64_t);
      event.diagnostic_payload[byte] = static_cast<char>(
          (fields[fieldIndex] >> (fieldByte * 8)) & 0xffu);
    }
    event.diagnostic_size = static_cast<std::uint16_t>(size);
    event.diagnostic_part = static_cast<std::uint16_t>(part);
    event.diagnostic_parts = static_cast<std::uint16_t>(eventCount);
    event.observation_mode = 4;
  }
  return events;
}

template <std::size_t FieldCount>
inline bool emit_detail_row(Source source, std::string_view schema,
    const DiagnosticContext& context,
    const std::array<std::uint64_t, FieldCount>& fields) noexcept {
  if (!context.generation || schema.empty() ||
      schema.size() >= sizeof(tracing::TraceEvent::diagnostic_schema)) return false;
  const auto events = make_detail_events(source, schema, context, fields);
  return tracing::emit_observations(events, context.generation);
}

}  // namespace dxmt9::observations
