#include "dxmt9/observations/detail_rows.hpp"
#include "dxmt9/observations/endpoint.hpp"

#include <array>
#include <cassert>
#include <cstdint>
#include <cstring>

namespace dxmt9::tracing {
std::atomic<bool> g_trace_enabled{true};
uint64_t capture_generation() noexcept { return 7; }
uint64_t now_ns() noexcept { return 123; }
uint64_t clock_resolution_ns() noexcept { return 1; }
}

namespace {
std::uint64_t decode_u64(const char* bytes) {
  std::uint64_t value = 0;
  for (std::size_t i = 0; i < sizeof(value); ++i)
    value |= static_cast<std::uint64_t>(static_cast<unsigned char>(bytes[i])) << (i * 8);
  return value;
}
}

int main() {
  using namespace dxmt9;
  observations::DiagnosticContext context;
  context.generation = 7;
  context.timestamp = 123;
  context.record = 9;
  context.work = 42;

  const std::array<std::uint64_t, observations::cpu_supply_row_fields.size()> cpu = {
      1, 2, 3, 4, 5, 6, 7, 8, 1, 10, 11, 12};
  const auto cpuEvents = observations::make_detail_events(
      observations::Source::CpuDetail, "cpu-supply-row-v2", context, cpu);
  static_assert(observations::detail_event_count<observations::cpu_supply_row_fields.size()> == 1);
  const auto& cpuEvent = cpuEvents[0];
  assert(cpuEvent.observation_mode == 4);
  assert(cpuEvent.diagnostic_parts == 1 && cpuEvent.diagnostic_part == 0);
  assert(cpuEvent.diagnostic_size == cpu.size() * sizeof(std::uint64_t));
  assert(std::strcmp(cpuEvent.diagnostic_schema, "cpu-supply-row-v2") == 0);
  assert(decode_u64(cpuEvent.diagnostic_payload) == 1);
  assert(decode_u64(cpuEvent.diagnostic_payload + 11 * sizeof(std::uint64_t)) == 12);

  std::array<std::uint64_t, observations::wait_row_fields.size()> wait{};
  for (std::size_t i = 0; i < wait.size(); ++i) wait[i] = 1000 + i;
  const auto waitEvents = observations::make_detail_events(
      observations::Source::WaitDetail, "wait-row-v2", context, wait);
  static_assert(observations::detail_event_count<observations::wait_row_fields.size()> == 2);
  assert(waitEvents[0].diagnostic_size == observations::detail_payload_capacity);
  assert(waitEvents[1].diagnostic_size ==
      wait.size() * sizeof(std::uint64_t) - observations::detail_payload_capacity);
  assert(waitEvents[0].diagnostic_part == 0 && waitEvents[1].diagnostic_part == 1);
  assert(waitEvents[0].diagnostic_parts == 2 && waitEvents[1].diagnostic_parts == 2);
  assert(decode_u64(waitEvents[0].diagnostic_payload) == 1000);
  assert(decode_u64(waitEvents[1].diagnostic_payload + 6 * sizeof(std::uint64_t)) ==
      wait.back());

  const observations::Endpoint endpoint{200, 190, 210, 2};
  const auto endpointEvent = observations::endpoint_event(
      observations::Source::WaitDetail, context, endpoint, "wait.mutex",
      tracing::Phase::Begin, 77);
  assert(endpointEvent.observation_mode == 5);
  assert(endpointEvent.diagnostic_size == 4 * sizeof(std::uint64_t));
  assert(decode_u64(endpointEvent.diagnostic_payload) == endpoint.source);
  assert(decode_u64(endpointEvent.diagnostic_payload + sizeof(std::uint64_t)) ==
      endpoint.before);
  assert(decode_u64(endpointEvent.diagnostic_payload + 2 * sizeof(std::uint64_t)) ==
      endpoint.after);
  assert(decode_u64(endpointEvent.diagnostic_payload + 3 * sizeof(std::uint64_t)) ==
      endpoint.resolution);
}
