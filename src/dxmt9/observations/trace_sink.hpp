#pragma once

#include "../dxmt9_trace.hpp"
#include <cstdint>

namespace dxmt9::observations {
enum class Source : std::uint16_t { CpuSupply = 1, Wait = 2 };

// A projection of an existing observation, not a second measurement. Native
// timestamp denotes the CPU-query start or wait-result observation. Original
// endpoint clocks and full predicates remain in the linked supplemental row.
inline bool counter(std::uint64_t generation, Source source,
                    std::uint64_t row, std::uint64_t timestamp,
                    tracing::Metric metric, std::uint64_t value) noexcept {
  if (!generation) return false;
  tracing::TraceEvent event{};
  event.timestamp_ns = timestamp;
  event.observation_record = row + 1;
  event.observation_kind = static_cast<std::uint16_t>(source);
  event.metric_id = static_cast<std::uint16_t>(metric);
  event.metric_value = value;
  event.event_id = static_cast<std::uint16_t>(tracing::EventId::Metric);
  event.category = static_cast<std::uint8_t>(source == Source::Wait
      ? tracing::Category::Wait : tracing::Category::Replay);
  event.phase = static_cast<std::uint8_t>(tracing::Phase::Counter);
  // emit_batch_captured prevents a late old observation entering a new session.
  if (value > INT64_MAX) {
    event.category = static_cast<std::uint8_t>(tracing::Category::Count);
    (void)tracing::emit_batch_captured({&event, 1}, generation, 1);
    return false;
  }
  return tracing::emit_observation(event, generation);
}
}  // namespace dxmt9::observations
