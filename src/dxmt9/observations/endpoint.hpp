#pragma once
#include "diagnostic_sink.hpp"

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
  std::snprintf(e.diagnostic_schema, sizeof(e.diagnostic_schema), "%s", schema);
  std::snprintf(e.diagnostic_payload, sizeof(e.diagnostic_payload),
      "source_ns=%llu native_before_ns=%llu native_after_ns=%llu resolution_ns=%llu qualified=1",
      static_cast<unsigned long long>(point.source), static_cast<unsigned long long>(point.before),
      static_cast<unsigned long long>(point.after), static_cast<unsigned long long>(point.resolution));
  e.diagnostic_parts = 1;
  e.observation_mode = 3;
  return e;
}
} // namespace dxmt9::observations
