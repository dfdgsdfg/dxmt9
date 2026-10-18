#include "dxmt9/observations/value_batch.hpp"

#include <array>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>

namespace dxmt9::tracing {
std::atomic<bool> g_trace_enabled{true};
std::atomic<bool> g_trace_detail_enabled{false};
std::atomic<std::uint64_t> g_trace_owner_registration_generation{0};

namespace {
std::vector<TraceEvent> emitted;
std::size_t batchCalls = 0;
std::size_t lossCalls = 0;
}

std::uint64_t capture_generation() noexcept { return 7; }
std::uint64_t now_ns() noexcept { return 100; }
bool emit_observations(std::span<const TraceEvent> events,
                       std::uint64_t generation) noexcept {
  assert(generation == 7);
  ++batchCalls;
  emitted.assign(events.begin(), events.end());
  return true;
}
bool emit_batch_captured(std::span<const TraceEvent> events,
                         std::uint64_t generation,
                         std::uint64_t lossCount) noexcept {
  assert(generation == 7);
  assert(events.size() == 1);
  assert(events[0].category == static_cast<std::uint8_t>(Category::Count));
  lossCalls += static_cast<std::size_t>(lossCount);
  return true;
}
}  // namespace dxmt9::tracing

int main() {
  using namespace dxmt9;
  using observations::DiagnosticContext;
  using observations::Source;
  const DiagnosticContext context{.generation = 7, .timestamp = 100,
                                  .record = 9, .work = 4};

  {
    observations::ValueBatch batch(context);
    for (std::uint64_t i = 0; i < 12; ++i) {
      char key[32]{};
      std::snprintf(key, sizeof(key), "row-%llu",
                    static_cast<unsigned long long>(i));
      batch.value(Source::PerfReport, key, i == 10 ? UINT64_MAX : i * 17,
                  "count:cumulative");
    }
    batch.commit();
  }
  assert(dxmt9::tracing::batchCalls == 1);
  assert(dxmt9::tracing::emitted.size() == 12);
  for (std::size_t i = 0; i < 12; ++i) {
    char key[32]{};
    std::snprintf(key, sizeof(key), "row-%llu",
                  static_cast<unsigned long long>(i));
    const auto& event = dxmt9::tracing::emitted[i];
    assert(std::strcmp(event.diagnostic_schema, key) == 0);
    assert(event.metric_value == (i == 10 ? UINT64_MAX : i * 17));
    assert(std::strcmp(event.diagnostic_payload, "count:cumulative") == 0);
  }

  {
    observations::ValueBatch batch(context);
    for (std::size_t i = 0; i <= observations::ValueBatch::capacity; ++i)
      batch.value(Source::PerfReport, "overflow", i, "count:cumulative");
    batch.commit();
  }
  assert(dxmt9::tracing::batchCalls == 1);
  assert(dxmt9::tracing::lossCalls == 1);

  {
    observations::ValueBatch first(context);
    observations::ValueBatch second(context);
    observations::ValueBatch contended(context);
    first.value(Source::PerfReport, "first", 1, "count:cumulative");
    second.value(Source::PerfReport, "second", 2, "count:cumulative");
    contended.value(Source::PerfReport, "third", 3, "count:cumulative");
    contended.commit();
    assert(dxmt9::tracing::lossCalls == 2);
    first.commit();
    second.commit();
  }
  assert(dxmt9::tracing::batchCalls == 3);
}
