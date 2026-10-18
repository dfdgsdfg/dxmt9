#pragma once
#include "diagnostic_sink.hpp"

#include <algorithm>
#include <cstddef>
#include <cstring>

namespace dxmt9::observations {
struct Endpoint {
  std::uint64_t source = 0, before = 0, after = 0;
  std::uint64_t resolution = 1;
  bool qualified() const noexcept {
    return source && before && resolution && resolution <= 500 && after >= before &&
        after - before <= 2000 - 4 * resolution;
  }
  std::uint64_t midpoint() const noexcept { return before + (after - before) / 2; }
};
template<typename Clock>
inline Endpoint endpoint(std::uint64_t generation, Clock&& clock) noexcept {
  Endpoint p;
  if (generation) { p.resolution = tracing::clock_resolution_ns(); p.before = tracing::now_ns(); }
  p.source = clock();
  if (generation) p.after = tracing::now_ns();
  return p;
}
inline tracing::TraceEvent endpoint_event(Source source, const DiagnosticContext& c,
    const Endpoint& point, const char* schema, tracing::Phase phase = tracing::Phase::Instant,
    std::uint64_t scope = 0) noexcept {
  auto e = diagnostic_event(source, c);
  e.event_id = static_cast<std::uint16_t>(tracing::EventId::DiagnosticEndpoint);
  e.timestamp_ns = point.midpoint();
  e.phase = static_cast<std::uint8_t>(phase);
  e.scope_id = scope;
  e.uncertainty_ns = (point.after - point.before + 1) / 2 + 2 * point.resolution;
  const auto schemaSize = std::min(std::strlen(schema), sizeof(e.diagnostic_schema) - 1);
  if (schemaSize) std::memcpy(e.diagnostic_schema, schema, schemaSize);
  const std::uint64_t fields[] = {point.source, point.before, point.after,
                                  point.resolution};
  constexpr std::size_t fieldBytes = sizeof(fields);
  for (std::size_t byte = 0; byte < fieldBytes; ++byte) {
    const auto fieldIndex = byte / sizeof(std::uint64_t);
    const auto fieldByte = byte % sizeof(std::uint64_t);
    e.diagnostic_payload[byte] = static_cast<char>(
        (fields[fieldIndex] >> (fieldByte * 8)) & 0xffu);
  }
  e.diagnostic_size = static_cast<std::uint16_t>(fieldBytes);
  e.diagnostic_parts = 1;
  e.observation_stride = sizeof(std::uint64_t);
  e.observation_mode = 5;
  return e;
}
} // namespace dxmt9::observations
