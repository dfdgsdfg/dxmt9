#include "../../../src/dxmt9/dxmt9_trace.hpp"
#include "../../../src/dxmt9/dxmt9_cpu_supply_trace.hpp"
#include "../../../src/dxmt9/dxmt9_wait_trace.hpp"
#include "../../../src/dxmt9/tracing/config.hpp"

#include "../../../src/dxmt9/dxmt9_query_poll_observer.hpp"
#include "../../../src/d3d9/device_c_query_receipt_observer.hpp"
#include "../../../src/dxmt9/dxmt9_append_diag.hpp"
#include "../../../src/dxmt9/observations/perf_snapshot.hpp"

#include "../../../src/dxmt9/observations/diagnostic_sink.hpp"
#include "../../../src/dxmt9/dxmt9_perf_counters_internal.hpp"
#include "dxmt9/device_c.h"
#include "../../../src/dxmt9/dxmt9_queue.hpp"
#include "../../../src/dxmt9/dxmt9_signposts.hpp"
#include "../../../src/dxmt9/dxmt9_query_timeline_observer.hpp"
#include "../../../src/d3d9/device_c_query_drain_observer.hpp"
#include "../../../src/util/diagnostic_file.hpp"
#include <CommonCrypto/CommonDigest.h>
#include <mach/mach_time.h>

#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>
#include <unistd.h>

#ifndef DXMT9_TEST_PERFETTO
#define DXMT9_TEST_PERFETTO 0
#endif

namespace {

std::string sha256(const std::string& text) {
  unsigned char digest[CC_SHA256_DIGEST_LENGTH];
  CC_SHA256(text.data(), static_cast<CC_LONG>(text.size()), digest);
  static constexpr char hex[] = "0123456789abcdef";
  std::string result;
  for (unsigned char byte : digest) {
    result.push_back(hex[byte >> 4]);
    result.push_back(hex[byte & 15]);
  }
  return result;
}

std::string hexBytes(const void* data, size_t size) {
  const auto* bytes = static_cast<const unsigned char*>(data);
  static constexpr char digits[] = "0123456789abcdef";
  std::string result;
  result.reserve(size * 2u);
  for (size_t i = 0; i < size; ++i) {
    result.push_back(digits[bytes[i] >> 4]);
    result.push_back(digits[bytes[i] & 0xf]);
  }
  return result;
}

#if DXMT9_TEST_PERFETTO
dxmt9::tracing::TraceEvent event(uint16_t event_id, uint8_t category,
                                 uint64_t timestamp, uint64_t thread_id) {
  dxmt9::tracing::TraceEvent result{};
  result.timestamp_ns = timestamp;
  result.work_id = 9001;
  result.arg0 = 77;
  result.arg1 = 88;
  result.process_id = static_cast<uint64_t>(getpid());
  result.thread_id = thread_id;
  result.clock_id = 3;
  result.event_id = event_id;
  result.category = category;
  result.phase = static_cast<uint8_t>(dxmt9::tracing::Phase::Instant);
  return result;
}
#endif

}  // namespace

int main(int argc, char** argv) {
  using namespace dxmt9::tracing;
  unsetenv("DXMT9_TRACE_CONFIG");
  setenv("DXMT_PERF_COUNTERS", "1", 1);
  setenv("DXMT_TRACE_QUEUE", "1", 1);
  setenv("DXMT9_PERF_PROCESS_MEMORY_PRESENTS", "1", 1);
  setenv("DXMT9_COMPAT_APPEND_DIAGNOSTICS", "2", 1);
  if (argc > 2) return 2;
  const bool preserve_artifacts = argc == 2;
  const auto parent = preserve_artifacts
      ? std::filesystem::path(argv[1])
      : std::filesystem::temp_directory_path();
  const auto base = parent /
      ("dxmt9-tracing-runtime-" + std::to_string(getpid()));
  std::string peRawHex;
  std::filesystem::create_directories(base);
  const char* drainModeValue = std::getenv("DXMT9_TEST_DRAIN_MODE");
  const std::string drainMode = drainModeValue ? drainModeValue : "deferred";
  if (drainMode != "deferred" && drainMode != "async") return 2;
  const auto supply_path = base / "cpu-supply.csv";
  const auto wait_path = base / "wait.csv";
  const char* collectorModeValue = std::getenv("DXMT9_TEST_COLLECTOR_MODE");
  const std::string collectorMode = collectorModeValue ? collectorModeValue : "dual";
  const auto unavailableSupplyPath = base / "missing-supply-dir" / "cpu-supply.csv";
  const auto unavailableWaitPath = base / "missing-wait-dir" / "wait.csv";
  const char* supplySelection = supply_path.c_str();
  const char* waitSelection = wait_path.c_str();
  if (collectorMode == "perfetto") {
    supplySelection = "perfetto";
    waitSelection = "perfetto";
  } else if (collectorMode == "unwritable") {
    supplySelection = unavailableSupplyPath.c_str();
    waitSelection = unavailableWaitPath.c_str();
  } else if (collectorMode != "dual" && collectorMode != "legacy-only") {
    return 2;
  }
  setenv("DXMT9_CPU_SUPPLY_TRACE", supplySelection, 1);
  setenv("DXMT9_CPU_SUPPLY_TRACE_FIRST_PRESENT", "1", 1);
  setenv("DXMT9_CPU_SUPPLY_TRACE_LAST_PRESENT", "2", 1);
  setenv("DXMT9_WAIT_TRACE", waitSelection, 1);
  setenv("DXMT9_WAIT_TRACE_FIRST_PRESENT", "1", 1);
  setenv("DXMT9_WAIT_TRACE_LAST_PRESENT", "2", 1);
  const auto config_path = base / "request.json";
  const auto trace_path = base / "trace.pftrace";
  const auto receipt_path = base / "receipt.json";
  const auto raw_path = base / "trace.pftrace.raw.jsonl";
  const std::string categories =
      "[\"import\",\"replay\",\"queue\",\"encode\",\"wait\",\"submit\",\"completion\",\"pe\"]";
  const std::string canonical =
      "{\"attempt_id\":\"attempt\",\"categories\":" + categories +
      ",\"limits\":{\"finalize_timeout_ms\":5000,\"max_bytes\":67108864,"
      "\"max_events\":65536},\"optional_collectors\":[],\"output\":{\"pftrace\":\"" +
      trace_path.string() + "\",\"receipt\":\"" + receipt_path.string() +
      "\"},\"profile_id\":\"profile\",\"question\":\"runtime-smoke\","
      "\"required_categories\":" + categories +
      ",\"required_collectors\":[\"engine-perfetto\"],\"run_id\":\"run\","
      "\"schema\":\"dxmt9.trace.request.v1\",\"transport\":{\"batch_events\":512,\"mode\":\"" +
      drainMode + "\",\"service_interval_ms\":1,\"transport_events\":16384},"
      "\"window\":{\"delay_ms\":0,"
      "\"duration_ms\":30000,\"mode\":\"duration\"}}";
  const std::string digest = sha256(canonical);
  const std::string request =
      "{\"schema\":\"dxmt9.trace.request.v1\",\"run_id\":\"run\","
      "\"attempt_id\":\"attempt\",\"profile_id\":\"profile\","
      "\"question\":\"runtime-smoke\",\"categories\":" + categories +
      ",\"required_categories\":" + categories +
      ",\"required_collectors\":[\"engine-perfetto\"],"
      "\"optional_collectors\":[],\"window\":{\"mode\":\"duration\","
      "\"delay_ms\":0,\"duration_ms\":30000},"
      "\"limits\":{\"max_events\":65536,\"max_bytes\":67108864,"
      "\"finalize_timeout_ms\":5000},\"output\":{\"pftrace\":\"" +
      trace_path.string() + "\",\"receipt\":\"" + receipt_path.string() +
      "\"},\"config_sha256\":\"" + digest + "\",\"transport\":{"
      "\"batch_events\":512,\"mode\":\"" + drainMode +
      "\",\"service_interval_ms\":1,\"transport_events\":16384}}";
  {
    std::ofstream out(config_path, std::ios::binary);
    out << request;
  }
  ParsedConfig parsed;
  std::string error;
  assert(parse_request(config_path.string(), parsed, error));
  assert(parsed.config_sha256 == digest);
  assert(parsed.raw_path == raw_path.string());

  assert(!initialize_from_environment());
  TraceConfig config{config_path.c_str(), receipt_path.c_str(),
                     raw_path.c_str(), trace_path.c_str()};
#if DXMT9_TEST_PERFETTO
  assert(prepare_and_start(config));
  for (unsigned attempt = 0; attempt < 500 && !enabled(); ++attempt)
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  assert(enabled());
  TraceEvent filtered{};
  filtered.category = static_cast<uint8_t>(Category::Detail);
  assert(!emit_observation(filtered, capture_generation()));
  filtered.category = static_cast<uint8_t>(Category::Replay);
  assert(!emit_observation(filtered, capture_generation() + 1));

  counter(Category::Queue, Metric::QueueReady, 0, 88);
  counter(Category::Queue, Metric::QueueReady, 17, 88);
  counter(Category::Queue, Metric::QueueReady, 9, 99);
  counter(Category::Replay, Metric::NativeThreadCpuNs, 123456789, 123);
  const bool legacyOnly = collectorMode == "legacy-only";
  if (legacyOnly) g_trace_enabled.store(false, std::memory_order_relaxed);
  dxmt9::supply::present(1);
  dxmt9::supply::record(dxmt9::supply::Event::RawReady, nullptr, 1);
  dxmt9::waittrace::present(1);
  auto waitRow = dxmt9::waittrace::begin(dxmt9::waittrace::Site::QueryQueue, nullptr, nullptr);
  waitRow.queue = 99;
  waitRow.acquired = waitRow.begin + 100000;
  waitRow.end = waitRow.acquired;
  dxmt9::waittrace::finish(waitRow);
  dxmt9::supply::present(4);
  dxmt9::waittrace::present(4);
  if (drainMode == "async") std::this_thread::sleep_for(std::chrono::milliseconds(2));
  if (legacyOnly) g_trace_enabled.store(true, std::memory_order_relaxed);
  {
    namespace backend = dxmt9::query::polldiag;
    backend::Sample sample;
    sample.timed = true;
    sample.last = 100;
    sample.at(backend::Phase::QueueWait, 137);
    backend::Totals totals;
    totals.add(sample, true, dxmt9::query::Poll::Pending);
    sample.at(backend::Phase::Exit, 1); // Invalid clocks retain census only.
    totals.add(sample, false, dxmt9::query::Poll::Failed);
    backend::project(totals, 2);
    backend::Totals counts;
    counts.add({}, false, dxmt9::query::Poll::Pending);
    backend::project(counts, 1);
    namespace receipt = dxmt9::d3d9::query_receipt_diag;
    receipt::Sample r;
    r.timed = true;
    r.route = receipt::Route::Handled;
    r.ns[0] = 53;
    receipt::Totals rt;
    rt.add(r, 1, 0);
    receipt::project(rt, 2);
    dxmt9::perf::CounterSnapshot previous, current;
    previous.drawCalls = 3;
    current.drawCalls = 10;
    previous.commandBuffers = 4; // Regression is explicit, never unsigned wrap.
    current.commandBuffers = 2;
    current.encodeDrawCpuNs = 1234;
    current.renderEncoderGpuLastPso = UINT64_MAX;
    dxmt9::observations::project_frame(123, previous, current);
    {
      dxmt9::append_diag::Scope parent(dxmt9::append_diag::Phase::Batch);
      dxmt9::append_diag::Scope child(dxmt9::append_diag::Phase::Append);
    }
    const auto acquired = dxmt9::append_diag::snapshot();
    assert(acquired[0].calls == 1 && acquired[2].calls == 1);
  }
  {
    using namespace dxmt9::observations;
    // Every family uses the same bounded admission and cold SQL representation.
    for (const auto source : {Source::QueryReport, Source::PerfReport, Source::ProcessMemory,
                             Source::PeReport, Source::QueueReport, Source::Signpost, Source::Renderer}) {
      assert(report(source, "shared-report-fixture-v1", "samples=11 omitted=3 identity=18446744073709551615"));
      if (drainMode == "async")
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    assert(value(Source::PerfReport, "exact-counter-fixture", UINT64_MAX, "id:latest"));
    dxmt9::perf::countPresentEncoded(); // One real OS memory sample, shared output.
    dxmt9::perf::detail::report(); // Entire production table, including ring populations.
    if (drainMode == "async") std::this_thread::sleep_for(std::chrono::milliseconds(2));
    D9CDiagnosticReport wire{};
    wire.structSize = sizeof(wire); wire.version = D9C_DIAGNOSTIC_REPORT_VERSION;
    wire.encoding = D9C_DIAGNOSTIC_ENCODING_TEXT;
    wire.sessionGeneration = capture_generation(); wire.nativeThreadId = native_thread_id();
    wire.guestProcessId = 777; wire.guestThreadId = 888;
    wire.processCreateTime100ns = 111; wire.threadCreateTime100ns = 222; wire.sourceOrder = 999;
    std::strcpy(wire.schema, "dxmt9-pe-sampler");
    std::strcpy(wire.payload, "samples=11 omitted_samples=3 overflow_samples=1 mode=top");
    wire.length = std::strlen(wire.payload);
    assert(dxmt9c_device_diagnostic_report(nullptr, &wire) == 0);
    wire.version = 1; assert(dxmt9c_device_diagnostic_report(nullptr, &wire) == 2);
    wire.version = D9C_DIAGNOSTIC_REPORT_VERSION; ++wire.sessionGeneration;
    assert(dxmt9c_device_diagnostic_report(nullptr, &wire) == 3);
    const uint64_t peRawFields[] = {
        1u, 17u, 0x1234u, 0x5678u, 0x1111222233334444ull,
        0x5555666677778888ull, 24'000'000u, 0x1234567890ull,
        0x12345678a0ull, 0xfedcba9876543210ull, 3u, 20u, 17u, 1u, 2u, 0u, 3u};
    peRawHex = hexBytes(peRawFields, sizeof(peRawFields));
    wire.sessionGeneration = capture_generation();
    wire.encoding = D9C_DIAGNOSTIC_ENCODING_U64_LE;
    wire.guestProcessId = 0x1234u; wire.guestThreadId = 0x5678u;
    wire.processCreateTime100ns = 0x1111222233334444ull;
    wire.threadCreateTime100ns = 0x5555666677778888ull;
    wire.sourceOrder = 91017u;
    std::strcpy(wire.schema, "pe-pc-sample-v1");
    std::memcpy(wire.payload, peRawFields, sizeof(peRawFields));
    wire.length = sizeof(peRawFields);
    assert(dxmt9c_device_diagnostic_report(nullptr, &wire) == 0);
    wire.length -= 1u;
    assert(dxmt9c_device_diagnostic_report(nullptr, &wire) == 2);
    wire.length += 1u;
    wire.encoding = 99u;
    assert(dxmt9c_device_diagnostic_report(nullptr, &wire) == 2);
    wire.encoding = D9C_DIAGNOSTIC_ENCODING_U64_LE;
    wire.version = 1u;
    assert(dxmt9c_device_diagnostic_report(nullptr, &wire) == 2);
    wire.version = D9C_DIAGNOSTIC_REPORT_VERSION;
    const uint64_t badRecordVersion = 2u;
    std::memcpy(wire.payload, &badRecordVersion, sizeof(badRecordVersion));
    assert(dxmt9c_device_diagnostic_report(nullptr, &wire) == 2);
    const std::string splitUtf8 = std::string(190, 'x') + "한글";
    assert(report(Source::Renderer, "unicode-fragment-fixture", splitUtf8));
    dxmt9::util::logf(dxmt9::util::LogLevel::Info, "dxmt9-query-timeline",
                     "queue=%llu query=%llu issue=%u", 17ull, 19ull, 2u);
    {
      dxmt9::d3d9::query_drain_diag::Ledger ledger;
      dxmt9::d3d9::query_drain_diag::Detail detail;
      detail.queryIdentity = 19; detail.drained = true; detail.drainNanoseconds = 1234;
      ledger.record(detail);
      ledger.report();
    }
    // A deterministic qualified endpoint pair exercises native interval export.
    DiagnosticContext c;
    const Endpoint first{100, c.timestamp, c.timestamp + 2};
    const Endpoint last{200, c.timestamp + 100, c.timestamp + 102};
    const TraceEvent endpoints[] = {
      endpoint_event(Source::WaitDetail, c, first, "fixture.wait", Phase::Begin, 910001),
      endpoint_event(Source::WaitDetail, c, last, "fixture.wait", Phase::End, 910001)};
    assert(emit_observations(endpoints, c.generation));
    dxmt9::core::metalqueue::QueueTraceSnapshot queue;
    queue.eventSeqId = 123; queue.readyCount = 7; queue.inflightCount = 2;
    dxmt9::core::metalqueue::traceQueueEvent("fixture", queue, "shared=1");
    dxmt9::signposts::tracePoint("fixture", 42, true, 123);
    dxmt9::signposts::tracePoint("fixture", 42, false, 123);
    const auto path = base / "renderer-structure.json";
    { dxmt9::diagnostics::OutputFile out(path.string(), std::ios::trunc);
      out << "{\"passes\":[{\"id\":18446744073709551615,\"draws\":3}]}"; }
  }
  const uint64_t base_time = now_ns();
  const uint64_t main_tid = native_thread_id();
  uint64_t peHostBase = 0u;
  {
    TraceScope scope(static_cast<uint8_t>(Category::Replay),
                     static_cast<uint16_t>(EventId::ReplayBegin),
                     static_cast<uint16_t>(EventId::ReplayEnd),
                     0, 9002, 77, 88);
    auto publication = event(static_cast<uint16_t>(EventId::QueuePublish),
                             static_cast<uint8_t>(Category::Queue), now_ns(), main_tid);
    publication.arg0 = 88;
    publication.arg1 = 0;
    assert(emit(publication));
    auto binding = event(static_cast<uint16_t>(EventId::ReplayPublish),
                         static_cast<uint8_t>(Category::Replay), now_ns(), main_tid);
    binding.work_id = 9002;
    binding.arg1 = 9001;
    binding.related_owner_id = 88;
    assert(emit(binding));

    // One publication may feed more than one encode invocation. Native flows
    // must preserve both exact endpoints without chaining the two children.
    for (unsigned i = 0; i < 2; ++i) {
      TraceScope encode(static_cast<uint8_t>(Category::Encode),
                        static_cast<uint16_t>(EventId::EncodeBegin),
                        static_cast<uint16_t>(EventId::EncodeEnd), 0, 9001, 88);
      auto member = event(static_cast<uint16_t>(EventId::SubmitMember),
                          static_cast<uint8_t>(Category::Submit), now_ns(), main_tid);
      member.arg0 = 88;
      member.arg1 = 99;
      assert(emit(member));
    }
  }

  // Exercise the PE point-bracket raw contract after the replay scope closes.
  // QPC at 24 MHz has a non-integral ns/tick scale, and each endpoint maps
  // only its exact source tick to the host midpoint.
  const uint64_t peGeneration = capture_generation();
  assert(peGeneration != 0u);
  constexpr uint64_t qpcFrequency = 24'000'000u;
  constexpr uint64_t sourceOrder = 9003u;
  constexpr uint64_t scopeId = 9003u;
  peHostBase = now_ns();
  mach_timebase_info_data_t timebase{};
  assert(mach_timebase_info(&timebase) == KERN_SUCCESS && timebase.denom != 0u);
  const uint64_t hostResolutionNs = std::max<uint64_t>(
      1u, timebase.numer / timebase.denom +
          (timebase.numer % timebase.denom != 0u));
  const uint64_t qpcResolutionNs =
      1'000'000'000u / qpcFrequency +
      (1'000'000'000u % qpcFrequency != 0u);
  const uint64_t pointErrorNs = 1u + qpcResolutionNs + hostResolutionNs;
  assert(pointErrorNs <= 1000u);
  const auto pePoint = [main_tid, pointErrorNs, hostResolutionNs](
                           uint16_t eventId, uint8_t phase, uint64_t qpc,
                           uint64_t hostBefore, uint64_t hostAfter,
                           uint64_t endpoint) {
    TraceEvent point{};
    point.timestamp_ns = hostBefore + (hostAfter - hostBefore) / 2u;
    point.track_id = 0u;
    point.work_id = sourceOrder;
    point.scope_id = scopeId;
    point.process_id = static_cast<uint64_t>(getpid());
    point.thread_id = main_tid;
    point.guest_process_id = 17u;
    point.guest_thread_id = 23u;
    point.process_create_time100ns = 1000u;
    point.thread_create_time100ns = 2000u;
    point.clock_frequency = qpcFrequency;
    point.uncertainty_ns = pointErrorNs;
    point.source_timestamp = qpc;
    point.source_clock_id = 0x44585150u;
    point.calibration_id = sourceOrder * 2u + endpoint;
    point.calibration_scale = 1'000'000'000.0 /
        static_cast<double>(qpcFrequency);
    point.calibration_offset = static_cast<double>(point.timestamp_ns) -
        static_cast<double>(qpc) * point.calibration_scale;
    point.calibration_source_first = qpc;
    point.calibration_host_first_ns = hostBefore;
    point.calibration_source_second = qpc;
    point.calibration_host_second_ns = hostAfter;
    point.calibration_valid_begin = qpc;
    point.calibration_valid_end = qpc;
    point.calibration_resolution_ns = hostResolutionNs;
    point.clock_id = 3u;
    point.event_id = eventId;
    point.category = static_cast<uint8_t>(Category::Pe);
    point.phase = phase;
    return point;
  };
  std::array<TraceEvent, 2> pePoints{
      pePoint(static_cast<uint16_t>(EventId::PeChunkCommitBegin),
              static_cast<uint8_t>(Phase::Begin), 24'000'001u,
              peHostBase - 2u, peHostBase, 0u),
      pePoint(static_cast<uint16_t>(EventId::PeChunkCommitEnd),
              static_cast<uint8_t>(Phase::End), 24'240'001u,
              peHostBase + 9'998u, peHostBase + 10'000u, 1u)};
  assert(emit_batch_captured(pePoints, peGeneration));

  std::array<std::thread, 4> producers;
  for (size_t i = 0; i < producers.size(); ++i) {
    producers[i] = std::thread([i, base_time] {
      const uint64_t tid = native_thread_id();
      for (uint16_t j = 0; j < 10; ++j) {
        const auto item = event(static_cast<uint16_t>(EventId::Completion),
                                static_cast<uint8_t>(Category::Completion),
                                base_time + 2000 + i * 100 + j, tid);
        assert(emit(item));
      }
    });
  }
  for (auto& producer : producers) producer.join();
  std::this_thread::sleep_for(std::chrono::milliseconds(5));
  assert(stop_and_finalize());
  {
    // A sample racing with close must count as omitted, not accepted.
    D9CDiagnosticReport closed{};
    closed.structSize = sizeof(closed);
    closed.version = D9C_DIAGNOSTIC_REPORT_VERSION;
    closed.encoding = D9C_DIAGNOSTIC_ENCODING_U64_LE;
    closed.length = D9C_PE_RAW_SAMPLE_BYTES;
    closed.sessionGeneration = peGeneration;
    closed.nativeThreadId = main_tid;
    closed.guestProcessId = closed.guestThreadId = 1;
    closed.processCreateTime100ns = closed.threadCreateTime100ns = 1;
    closed.sourceOrder = 1;
    std::strcpy(closed.schema, "pe-pc-sample-v1");
    closed.payload[0] = 1;
    assert(dxmt9c_device_diagnostic_report(nullptr, &closed) == 4);
  }
  const auto proof_path = std::filesystem::path(receipt_path.string() + ".completion.json");
  const auto proof_deadline = std::chrono::steady_clock::now() +
      std::chrono::seconds(5);
  while (!std::filesystem::exists(proof_path) &&
         std::chrono::steady_clock::now() < proof_deadline)
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  assert(std::filesystem::exists(proof_path));
  std::ifstream completionProof(proof_path, std::ios::binary);
  const std::string proofText(std::istreambuf_iterator<char>(completionProof), {});
  assert(proofText.find("\"schema\":\"dxmt9.trace.completion.v1\"") !=
      std::string::npos);
  assert(proofText.find("\"status\":\"complete\"") != std::string::npos);
  assert(proofText.find("\"run_id\":\"run\"") != std::string::npos);
  assert(proofText.find("\"attempt_id\":\"attempt\"") != std::string::npos);
  assert(proofText.find("\"config_sha256\":\"" + digest + "\"") !=
      std::string::npos);

  assert(std::filesystem::file_size(trace_path) > 0);
  assert(std::filesystem::file_size(raw_path) > 0);
  std::ifstream raw(raw_path, std::ios::binary);
  const std::string raw_text(std::istreambuf_iterator<char>(raw), {});
  assert(raw_text.find("\"schema\":\"dxmt9.trace.raw.v1\"") != std::string::npos);
  assert(raw_text.find("\"diagnostic_schema\":\"pe-pc-sample-v1\"") !=
         std::string::npos);
  assert(raw_text.find("\"diagnostic_payload\":\"" + peRawHex + "\"") !=
         std::string::npos);
  assert(raw_text.find("\"diagnostic_encoding\":\"4\"") !=
         std::string::npos);
  assert(raw_text.find("\"observation_record\":\"91017\"") !=
         std::string::npos);
  const bool hasSupplyDetail =
      raw_text.find("cpu-supply-row-v2") != std::string::npos;
  const bool hasWaitDetail = raw_text.find("wait-row-v2") != std::string::npos;
  if (collectorMode == "legacy-only") {
    assert(!hasSupplyDetail && !hasWaitDetail);
  } else {
    assert(hasSupplyDetail && hasWaitDetail);
  }
  if (collectorMode == "dual" || collectorMode == "legacy-only") {
    assert(std::filesystem::exists(supply_path));
    assert(std::filesystem::exists(wait_path));
    std::ifstream supplyCsv(supply_path, std::ios::binary);
    std::ifstream waitCsv(wait_path, std::ios::binary);
    const std::string supplyText(std::istreambuf_iterator<char>(supplyCsv), {});
    const std::string waitText(std::istreambuf_iterator<char>(waitCsv), {});
    assert(supplyText.find("wall_ns,cpu_ns,tid,owner,id,value,frame,event") !=
        std::string::npos);
    assert(waitText.find("site,owner,target,query,queue,sequence") !=
        std::string::npos);
  } else {
    assert(!std::filesystem::exists(supply_path));
    assert(!std::filesystem::exists(wait_path));
  }
  assert(raw_text.find("\"relations\":[{\"relation_id\":") != std::string::npos);
  assert(raw_text.find("\"work_ids\":[\"9001\"]") != std::string::npos);
  const std::string pointMapping = "\"mapping_kind\":\"point_bracket\"";
  const size_t firstPointMapping = raw_text.find(pointMapping);
  assert(firstPointMapping != std::string::npos);
  assert(raw_text.find(pointMapping,
                       firstPointMapping + pointMapping.size()) !=
         std::string::npos);
  assert(raw_text.find("\"calibration_resolution_ns\":\"" +
                       std::to_string(hostResolutionNs) + "\"") !=
         std::string::npos);
  assert(raw_text.find("\"scale\":\"41.666") !=
         std::string::npos);
  assert(raw_text.find("\"calibration_id\":\"18006\"") !=
         std::string::npos);
  assert(raw_text.find("\"calibration_id\":\"18007\"") !=
         std::string::npos);
  assert(raw_text.find("\"frequency\":\"24000000\"") !=
         std::string::npos);
  assert(raw_text.find("\"source_ticks\":\"24000001\"") !=
         std::string::npos);
  assert(raw_text.find("\"source_ticks\":\"24240001\"") !=
         std::string::npos);
  assert(raw_text.find("\"source_before\":\"24000001\"") !=
         std::string::npos);
  assert(raw_text.find("\"source_after\":\"24240001\"") !=
         std::string::npos);
  assert(raw_text.find("\"host_ns\":\"" +
                       std::to_string(peHostBase - 1u) + "\"") !=
         std::string::npos);
  assert(raw_text.find("\"host_ns\":\"" +
                       std::to_string(peHostBase + 9'999u) + "\"") !=
         std::string::npos);
  assert(raw_text.find("\"host_before_ns\":\"" +
                       std::to_string(peHostBase - 2u) + "\"") !=
         std::string::npos);
  assert(raw_text.find("\"host_before_ns\":\"" +
                       std::to_string(peHostBase + 9'998u) + "\"") !=
         std::string::npos);
  assert(raw_text.find("\"host_after_ns\":\"" +
                       std::to_string(peHostBase + 10'000u) + "\"") !=
         std::string::npos);
  assert(raw_text.find("\"track_id\":\"" +
                       std::to_string((peGeneration << 32) | 1u) + "\"") !=
         std::string::npos);
  assert(raw_text.find("\"native_tid\":\"" +
                       std::to_string(main_tid) + "\"") !=
         std::string::npos);
  const auto assertPeEventTrack = [&](uint16_t eventId) {
    const std::string eventType = "\"event_type\":" +
        std::to_string(eventId);
    const size_t eventPosition = raw_text.find(eventType);
    assert(eventPosition != std::string::npos);
    const std::string track = "\"track_id\":\"" +
        std::to_string((peGeneration << 32) | 1u) + "\"";
    const size_t trackPosition = raw_text.find(track, eventPosition);
    assert(trackPosition != std::string::npos);
  };
  assertPeEventTrack(static_cast<uint16_t>(EventId::PeChunkCommitBegin));
  assertPeEventTrack(static_cast<uint16_t>(EventId::PeChunkCommitEnd));
  assert(raw_text.find("\"binding_kind\":\"raw_publication\"") !=
         std::string::npos);
  assert(raw_text.find("\"phase\":\"counter\"") != std::string::npos);
  if (collectorMode == "legacy-only") {
    assert(raw_text.find("\"observation_source\":\"cpu-supply\"") == std::string::npos);
    assert(raw_text.find("\"observation_source\":\"wait\"") == std::string::npos);
  } else {
    assert(raw_text.find("\"observation_source\":\"cpu-supply\"") != std::string::npos);
    assert(raw_text.find("\"observation_source\":\"wait\"") != std::string::npos);
    assert(raw_text.find("wait_mutex_acquire_elapsed_ns") != std::string::npos);
  }
  assert(raw_text.find("\"counter_value\":\"17\"") != std::string::npos);
  assert(raw_text.find("queue_ready/owner/99/thread/") != std::string::npos);
  assert(raw_text.find("observations.sqlite") != std::string::npos);
  assert(raw_text.find("\"phase\":\"begin\"") != std::string::npos);
  assert(raw_text.find("\"phase\":\"end\"") != std::string::npos);
  assert(raw_text.find("\"scope_id\":\"") != std::string::npos);
  std::ifstream receipt(receipt_path, std::ios::binary);
  const std::string receipt_text(std::istreambuf_iterator<char>(receipt), {});
  assert(receipt_text.find("\"status\":\"complete\"") != std::string::npos);
  assert(receipt_text.find("\"flush\":{\"status\":\"complete\"}") !=
         std::string::npos);
  assert(proofText.find("\"candidate_receipt_sha256\":\"" +
      sha256(receipt_text) + "\"") != std::string::npos);
  if (!preserve_artifacts) std::filesystem::remove_all(base);
  std::cout << "Tracing runtime smoke passed\n";
#else
  assert(!prepare_and_start(config));
  assert(!enabled());
  if (!preserve_artifacts) std::filesystem::remove_all(base);
  std::cout << "Tracing SDK-off facade passed\n";
#endif
}
