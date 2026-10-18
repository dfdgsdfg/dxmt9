#include "dxmt9/observations/diagnostic_sink.hpp"
#include "dxmt9/observations/endpoint.hpp"
#include "dxmt9/dxmt9_perf_counters.hpp"
#include "dxmt9/dxmt9_perf_counters_internal.hpp"
#include "dxmt9/diagnostic_report.h"
#include <cassert>
#include <vector>
#include <cstring>
#include <map>
#include <thread>
#include "dxmt9/observations/reservation.hpp"
#include "util/diagnostic_file.hpp"

namespace { std::vector<dxmt9::tracing::TraceEvent> captured; uint64_t losses=0, loads=0; bool accepting=true; }
namespace dxmt9::tracing {
std::atomic<bool> g_trace_enabled{true};
uint64_t capture_generation() noexcept { return 7; }
uint64_t now_ns() noexcept { return 123; }
uint64_t clock_resolution_ns() noexcept { return 1; }
bool emit_observations(std::span<const TraceEvent> events, uint64_t generation) noexcept {
  if (!accepting || generation != 7) return false;
  captured.insert(captured.end(), events.begin(), events.end()); return true;
}
bool emit_observation(const TraceEvent& event, uint64_t generation) noexcept {
  return emit_observations({&event, 1}, generation);
}
bool emit_batch_captured(std::span<const TraceEvent>, uint64_t generation, uint64_t count) noexcept {
  if (generation == 7) losses += count; return false;
}
}
namespace dxmt9::perf {
bool enabled() { return true; }
namespace detail {
Counters& counters() { static Counters result; return result; }
uint64_t load(const std::atomic<uint64_t>& value) { ++loads; return value.load(); }
}
}
int main() {
  using namespace dxmt9;
  using namespace observations;
  {
    std::atomic<uint32_t> cursor{0};
    std::array<std::atomic<unsigned>, 1024> slots{};
    std::vector<std::thread> workers;
    for (unsigned thread = 0; thread < 8; ++thread) workers.emplace_back([&] {
      uint32_t first = 0;
      while (reserve(cursor, slots.size(), 4, first))
        for (unsigned j = 0; j < 4; ++j) ++slots[first + j];
    });
    for (auto& worker : workers) worker.join();
    assert(cursor == slots.size());
    for (auto& slot : slots) assert(slot == 1);
    uint32_t first = 0;
    assert(!reserve(cursor, slots.size(), UINT32_MAX, first));
    assert(cursor == slots.size());
  }
  DiagnosticContext context;
  install_diagnostic_consumer();
  captured.clear();
  const std::string structuralJson =
      "{\"structural_kind\":\"fixture\",\"seq\":18446744073709551615}";
  {
    // /dev/null is not a directory, so the artifact open must fail while the
    // bounded owned tee still publishes the exact structured observation.
    diagnostics::StructuredOutputFile out(
        "/dev/null/dxmt9-structural-fixture.json", std::ios::trunc,
        "dxmt9-structural-json-v1");
    out << structuralJson;
  }
  std::string structuralJoined;
  bool foundStructural = false;
  for (const auto& event : captured) {
    if (std::string_view(event.diagnostic_schema) ==
        "dxmt9-structural-json-v1") {
      structuralJoined.append(event.diagnostic_payload,
                              std::strlen(event.diagnostic_payload));
      foundStructural = true;
    }
  }
  assert(foundStructural && structuralJoined == structuralJson);
  captured.clear();
  std::string payload(500, 'a');
  assert(report(Source::QueryReport, "query", payload, context));
  assert(captured.size()==3);
  std::string joined;
  for (size_t i=0;i<captured.size();++i) {
    assert(captured[i].diagnostic_part==i && captured[i].diagnostic_parts==3);
    assert(captured[i].observation_record==context.record);
    joined += captured[i].diagnostic_payload;
  }
  assert(joined==payload);
  const auto size=captured.size();
  assert(!report(Source::QueryReport, "query", std::string(12225,'x'), context));
  assert(losses==1 && captured.size()==size);
  assert(!report(Source::QueryReport, "query", "truncated", context, true));
  assert(losses==2);
  auto stale=context; stale.generation=6;
  assert(!report(Source::QueryReport,"query","stale",stale));
  accepting=false;
  assert(!report(Source::Renderer,"renderer","full",context));
  accepting=true;
  tracing::g_trace_enabled=false;
  assert(!report(Source::Renderer,"renderer","disabled"));
  tracing::g_trace_enabled=true;
  const Endpoint good{1,100,110}, reversed{1,110,100}, wide{1,100,2100}, missing{};
  assert(good.qualified() && good.midpoint()==105);
  assert(!reversed.qualified() && !wide.qualified() && !missing.qualified());
  D9CDiagnosticReport wire{};
  wire.structSize=sizeof(wire); wire.version=D9C_DIAGNOSTIC_REPORT_VERSION;
  wire.encoding=D9C_DIAGNOSTIC_ENCODING_TEXT;
  wire.sessionGeneration=wire.nativeThreadId=wire.guestProcessId=wire.guestThreadId=1;
  wire.processCreateTime100ns=wire.threadCreateTime100ns=wire.sourceOrder=1;
  std::strcpy(wire.schema,"sampler"); std::strcpy(wire.payload,"samples=3"); wire.length=9;
  assert(dxmt9c_diagnostic_report_validate(&wire)==0);
  wire.length=4096; assert(dxmt9c_diagnostic_report_validate(&wire)); wire.length=9;
  wire.version=1; assert(dxmt9c_diagnostic_report_validate(&wire));
  wire.version=D9C_DIAGNOSTIC_REPORT_VERSION;
  wire.encoding=D9C_DIAGNOSTIC_ENCODING_U64_LE;
  std::memset(wire.payload, 0, sizeof(wire.payload)); wire.length=16;
  assert(dxmt9c_diagnostic_report_validate(&wire)==0);
  wire.length=15; assert(dxmt9c_diagnostic_report_validate(&wire));
  wire.length=16; wire.encoding=D9C_DIAGNOSTIC_ENCODING_TEXT;
  wire.schema[127]='x'; std::memset(wire.schema,'x',127);
  assert(dxmt9c_diagnostic_report_validate(&wire));
  captured.clear();
  auto& counters=perf::detail::counters();
  counters.chunkAdmit=UINT64_MAX;
  counters.submitPresentCpuNs=1234567;
  counters.submitPresentCpuRing.record(9876543);
  perf::detail::report();
  std::map<std::string, uint64_t> fields;
  size_t typed=0;
  for (const auto& e:captured) if(e.observation_mode==2) {
    fields[e.diagnostic_schema]=e.metric_value; ++typed;
  }
  assert(fields.at("chunk_admit")==UINT64_MAX);
  assert(fields.at("submit_present_cpu_ms")==1234567);
  assert(fields.at("submit_present_cpu_p50_ms")==9876543);
  assert(typed>2300 && loads>2000);
  // Source-defined table fields remain exact even when legacy rendering rounds.
  assert(losses==2);
}
