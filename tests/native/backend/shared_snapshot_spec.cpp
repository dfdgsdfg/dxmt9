#include "../../../src/dxmt9/dxmt9_query_poll_observer.hpp"
#include "../../../src/d3d9/device_c_query_receipt_observer.hpp"
#include "../../../src/dxmt9/observations/perf_snapshot.hpp"
#include <cassert>
#include <string_view>
#include <vector>

// Counting sink proves adapters neither poll nor measure phase clocks again.
namespace dxmt9::tracing {
std::atomic<bool> g_trace_enabled{true};
static std::uint64_t generation = 7, clockReads = 0, losses = 0;
static bool selected = true;
static std::vector<TraceEvent> records;
std::uint64_t capture_generation() noexcept { return generation; }
std::uint64_t now_ns() noexcept { ++clockReads; return 123456; }
bool emit_observation(const TraceEvent& event, std::uint64_t captured) noexcept {
  if (captured != generation || !selected) return false;
  records.push_back(event);
  return true;
}
bool emit_observations(std::span<const TraceEvent> events, std::uint64_t captured) noexcept {
  if (captured != generation || !selected) return false;
  records.insert(records.end(), events.begin(), events.end());
  return true;
}
bool emit_batch_captured(std::span<const TraceEvent>, std::uint64_t captured,
                         std::uint64_t count) noexcept {
  if (captured != generation) return false;
  losses += count;
  return true;
}
}

int main() {
  using namespace dxmt9;
  using namespace tracing;
  namespace backend = query::polldiag;
  backend::Sample s;
  s.timed = true;
  s.last = 10;
  s.at(backend::Phase::QueueWait, 47);
  backend::Totals totals;
  totals.add(s, true, query::Poll::Pending);
  s.at(backend::Phase::Exit, 1);
  totals.add(s, false, query::Poll::Failed);
  backend::project(totals, 2);
  assert(records.size() == std::size(observations::backend_fields));
  assert(clockReads == 1 && records[0].metric_value == 2);
  assert(records[2].metric_value == 1 && records[3].metric_value == 1);
  assert(records[15].metric_value == 37 && records[23].metric_value == 37);
  for (const auto& e : records) {
    assert(e.timestamp_ns == 123456 && e.observation_record == 1);
    assert(e.observation_mode == 2 && e.observation_stride == 64);
  }
  records.clear();
  backend::Totals counts;
  counts.add({}, false, query::Poll::Pending);
  backend::project(counts, 1);
  assert(records[0].metric_value == 1 && records[2].metric_value == 0);
  assert(records[0].observation_stride == 0 && records[15].metric_value == 0);

  namespace receipt = d3d9::query_receipt_diag;
  receipt::Sample r;
  r.timed = true; r.ns[0] = 53; r.route = receipt::Route::Handled;
  receipt::Totals rt;
  rt.add(r, 1, 0);
  records.clear();
  receipt::project(rt, 2);
  assert(records.size() == std::size(observations::receipt_fields));
  assert(records[0].metric_value == 1 && records[12].metric_value == 1);
  assert(records[27].metric_value == 53 && records[32].metric_value == 53);

  perf::CounterSnapshot previous, current;
  previous.drawCalls = 3; current.drawCalls = 10;
  previous.commandBuffers = 4; current.commandBuffers = 2;
  current.renderEncoderGpuLastPso = UINT64_MAX;
  records.clear();
  observations::project_frame(123, previous, current);
  assert(records.size() == std::size(observations::perf_fields));
  bool identity = false, draw = false;
  for (const auto& e : records) {
    const auto f = observations::field(e.observation_kind, e.observation_field);
    if (std::string_view(f.name) == "draw_calls") {
      assert(e.metric_value == 7); draw = true;
    }
    if (std::string_view(f.name) == "render_encoder_gpu_last_pso") {
      assert(e.metric_value == UINT64_MAX && e.phase == std::uint8_t(Phase::Instant));
      identity = true;
    }
    assert(e.work_id == 123);
  }
  assert(identity && draw && records.back().metric_value == 1);

  const auto complete = [](observations::SnapshotSink& sink) {
    bool accepted = false;
    for (std::uint16_t i = 0; i < std::size(observations::backend_fields); ++i)
      accepted = sink.value(i, 1);
    return accepted;
  };
  observations::SnapshotSink old(observations::Source::QueryBackend, 2, 64);
  observations::SnapshotSink oldInvalid(observations::Source::QueryBackend, 2, 64);
  ++generation;
  records.clear();
  assert(!complete(old) && records.empty());
  assert(!oldInvalid.value(0, UINT64_MAX) && losses == 0);
  observations::SnapshotSink filtered(observations::Source::QueryBackend, 1, 0);
  selected = false;
  assert(!complete(filtered) && records.empty());
  selected = true;
  observations::SnapshotSink numeric(observations::Source::QueryBackend, 1, 0);
  assert(!numeric.value(0, UINT64_MAX) && losses == 1 && records.empty());
  observations::SnapshotSink invalidField(observations::Source::QueryBackend, 1, 0);
  assert(!invalidField.value(UINT16_MAX, 1) && losses == 2);
  observations::SnapshotSink badSource(static_cast<observations::Source>(99), 0, 0);
  assert(!badSource.value(0, 1));
  {
    observations::SnapshotSink partial(observations::Source::QueryBackend, 1, 0);
    assert(partial.value(0, 1));
  }
  assert(records.empty());

  // Production ledger cadence: first call, periodic 4096 and final snapshot.
  setenv("DXMT9_QUERY_BACKEND_OBSERVER", "counts", 1);
  records.clear();
  {
    backend::Ledger ledger;
    for (unsigned i = 0; i < 4096; ++i) ledger.add({}, false, query::Poll::Pending);
    assert(records.size() == 2 * std::size(observations::backend_fields));
    assert(records[0].metric_value == 1);
    assert(records[std::size(observations::backend_fields)].metric_value == 4096);
  }
  assert(records.size() == 3 * std::size(observations::backend_fields));
  records.clear();
  g_trace_enabled = false;
  const auto reads = clockReads;
  backend::project(totals, 2);
  observations::SnapshotSink off(observations::Source::PerfFrame, 1, 0);
  assert(!off.value(0, 1) && clockReads == reads && records.empty());
}
