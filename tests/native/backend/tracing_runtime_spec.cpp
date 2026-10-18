#include "../../../src/dxmt9/dxmt9_trace.hpp"
#include "../../../src/dxmt9/tracing/config.hpp"

#include <CommonCrypto/CommonDigest.h>
#include <mach/mach_time.h>

#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>
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
  if (argc > 2) return 2;
  const bool preserve_artifacts = argc == 2;
  const auto parent = preserve_artifacts
      ? std::filesystem::path(argv[1])
      : std::filesystem::temp_directory_path();
  const auto base = parent /
      ("dxmt9-tracing-runtime-" + std::to_string(getpid()));
  std::filesystem::create_directories(base);
  const auto config_path = base / "request.json";
  const auto trace_path = base / "trace.pftrace";
  const auto receipt_path = base / "receipt.json";
  const auto raw_path = base / "trace.pftrace.raw.jsonl";
  const std::string categories =
      "[\"import\",\"replay\",\"queue\",\"encode\",\"wait\",\"submit\",\"completion\",\"pe\"]";
  const std::string canonical =
      "{\"attempt_id\":\"attempt\",\"categories\":" + categories +
      ",\"limits\":{\"finalize_timeout_ms\":5000,\"max_bytes\":1048576,"
      "\"max_events\":4096},\"optional_collectors\":[],\"output\":{\"pftrace\":\"" +
      trace_path.string() + "\",\"receipt\":\"" + receipt_path.string() +
      "\"},\"profile_id\":\"profile\",\"question\":\"runtime-smoke\","
      "\"required_categories\":" + categories +
      ",\"required_collectors\":[\"engine-perfetto\"],\"run_id\":\"run\","
      "\"schema\":\"dxmt9.trace.request.v1\",\"window\":{\"delay_ms\":0,"
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
      "\"limits\":{\"max_events\":4096,\"max_bytes\":1048576,"
      "\"finalize_timeout_ms\":5000},\"output\":{\"pftrace\":\"" +
      trace_path.string() + "\",\"receipt\":\"" + receipt_path.string() +
      "\"},\"config_sha256\":\"" + digest + "\"}";
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

  counter(Category::Queue, Metric::QueueReady, 0, 88);
  counter(Category::Queue, Metric::QueueReady, 17, 88);
  counter(Category::Queue, Metric::QueueReady, 9, 99);
  counter(Category::Replay, Metric::NativeThreadCpuNs, 123456789, 0);
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

  assert(std::filesystem::file_size(trace_path) > 0);
  assert(std::filesystem::file_size(raw_path) > 0);
  std::ifstream raw(raw_path, std::ios::binary);
  const std::string raw_text(std::istreambuf_iterator<char>(raw), {});
  assert(raw_text.find("\"schema\":\"dxmt9.trace.raw.v1\"") != std::string::npos);
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
  if (!preserve_artifacts) std::filesystem::remove_all(base);
  std::cout << "Tracing runtime smoke passed\n";
#else
  assert(!prepare_and_start(config));
  assert(!enabled());
  if (!preserve_artifacts) std::filesystem::remove_all(base);
  std::cout << "Tracing SDK-off facade passed\n";
#endif
}
