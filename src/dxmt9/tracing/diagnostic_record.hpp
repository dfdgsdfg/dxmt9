#pragma once

#include <cstring>

namespace dxmt9::tracing {

// Validate copied record bounds before either cold serializer treats these
// arrays as strings or uses the explicit binary payload length.
template<class Event>
bool validDiagnosticRecord(const Event& event) noexcept {
  if (!std::memchr(event.diagnostic_schema, '\0', sizeof(event.diagnostic_schema)))
    return false;
  if (event.diagnostic_size > sizeof(event.diagnostic_payload) ||
      event.diagnostic_parts > 64) return false;
  if (!event.diagnostic_parts)
    return event.diagnostic_part == 0 && event.diagnostic_size == 0;
  if (event.diagnostic_part >= event.diagnostic_parts) return false;
  return event.observation_mode >= 4 ||
      std::memchr(event.diagnostic_payload, '\0', sizeof(event.diagnostic_payload));
}

}  // namespace dxmt9::tracing
