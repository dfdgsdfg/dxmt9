#include "../../../src/dxmt9/dxmt9_trace.hpp"
#include "../../../src/dxmt9/tracing/config.hpp"

#include <CommonCrypto/CommonDigest.h>

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
      "[\"import\",\"replay\",\"queue\",\"encode\",\"wait\",\"submit\",\"completion\"]";
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
