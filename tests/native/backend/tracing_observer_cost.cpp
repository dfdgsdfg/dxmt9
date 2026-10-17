#include "../../../src/dxmt9/dxmt9_trace.hpp"
#include "../../../src/dxmt9/tracing/artifact.hpp"
#include "../../../src/dxmt9/tracing/config.hpp"

#include <CommonCrypto/CommonDigest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <time.h>
#include <thread>
#include <unistd.h>

namespace {

constexpr uint32_t kRepeatCount = 3;
constexpr uint32_t kEventsPerRepeat = 256;
constexpr uint64_t kCpuOverheadBudgetNsPerEvent = 10'000;
constexpr uint32_t kMaxEvents = 196'608;
constexpr uint32_t kMaxBytes = 64 * 1024 * 1024;

std::atomic<uint64_t> app_sink{0};

std::string sha256(std::string_view text) {
  unsigned char digest[CC_SHA256_DIGEST_LENGTH];
  CC_SHA256(text.data(), static_cast<CC_LONG>(text.size()), digest);
  static constexpr char hex[] = "0123456789abcdef";
  std::string result;
  result.reserve(CC_SHA256_DIGEST_LENGTH * 2);
  for (unsigned char byte : digest) {
    result.push_back(hex[byte >> 4]);
    result.push_back(hex[byte & 15]);
  }
  return result;
}

uint64_t thread_cpu_ns() {
  timespec value{};
  if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &value) != 0) return 0;
  return static_cast<uint64_t>(value.tv_sec) * 1'000'000'000ull +
      static_cast<uint64_t>(value.tv_nsec);
}

uint64_t application_work(uint32_t index, uint64_t value) {
  value ^= static_cast<uint64_t>(index) + 0x9e3779b97f4a7c15ull;
  value = (value << 13) | (value >> 51);
  value *= 0xbf58476d1ce4e5b9ull;
  value ^= value >> 29;
  return value;
}

struct Measurement {
  uint64_t wall_ns = 0;
  uint64_t thread_cpu_ns = 0;
  uint64_t rejected_events = 0;
  uint64_t application_checksum = 0;
};

__attribute__((noinline)) Measurement measure_repeat(bool cached_gate,
                                                      uint32_t event_count) {
  Measurement result{};
  uint64_t app_value = 0x243f6a8885a308d3ull;
  const auto wall_start = std::chrono::steady_clock::now();
  const uint64_t cpu_start = thread_cpu_ns();
  for (uint32_t i = 0; i < event_count; ++i) {
    app_value = application_work(i, app_value);
    if (cached_gate) {
      dxmt9::tracing::TraceEvent event{};
      event.work_id = app_value;
      event.event_id = static_cast<uint16_t>(
          dxmt9::tracing::EventId::DetailDraw);
      event.category = static_cast<uint8_t>(dxmt9::tracing::Category::Detail);
      event.phase = static_cast<uint8_t>(dxmt9::tracing::Phase::Instant);
      if (!dxmt9::tracing::emit(event)) ++result.rejected_events;
    }
  }
  const uint64_t cpu_end = thread_cpu_ns();
  const auto wall_end = std::chrono::steady_clock::now();
  result.thread_cpu_ns = cpu_end >= cpu_start ? cpu_end - cpu_start : 0;
  result.wall_ns = static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(wall_end - wall_start)
          .count());
  result.application_checksum = app_value;
  app_sink.store(app_value, std::memory_order_relaxed);
  return result;
}

uint64_t parse_receipt_integer(const std::string& receipt, const char* key) {
  const std::string marker = std::string("\"") + key + "\":\"";
  const std::size_t begin = receipt.find(marker);
  if (begin == std::string::npos) return UINT64_MAX;
  const std::size_t value_begin = begin + marker.size();
  const std::size_t end = receipt.find('"', value_begin);
  if (end == std::string::npos) return UINT64_MAX;
  try {
    return std::stoull(receipt.substr(value_begin, end - value_begin));
  } catch (...) {
    return UINT64_MAX;
  }
}

bool write_file(const std::filesystem::path& path, std::string_view contents) {
  dxmt9::tracing::detail::AtomicArtifact output(path.string());
  return output.open() && output.write(contents.data(), contents.size()) &&
      output.commit();
}

}  // namespace

int main(int argc, char** argv) {
  using namespace dxmt9::tracing;
  if (argc != 2 || !std::filesystem::path(argv[1]).is_absolute()) return 2;
  unsetenv("DXMT9_TRACE_CONFIG");

  const auto output_root = std::filesystem::path(argv[1]) /
      ("dxmt9-observer-cost-" + std::to_string(getpid()));
  std::filesystem::create_directories(output_root);
  const auto request_path = output_root / "request.json";
  const auto trace_path = output_root / "trace.pftrace";
  const auto receipt_path = output_root / "receipt.json";
  const auto raw_path = std::filesystem::path(trace_path.string() + ".raw.jsonl");
  const auto report_path = output_root / "observer-cost.json";

  const std::string categories = "[\"detail\"]";
  const std::string canonical =
      "{\"attempt_id\":\"observer-cost\",\"categories\":" + categories +
      ",\"limits\":{\"finalize_timeout_ms\":10000,\"max_bytes\":" +
      std::to_string(kMaxBytes) + ",\"max_events\":" +
      std::to_string(kMaxEvents) + "},\"optional_collectors\":[],"
      "\"output\":{\"pftrace\":\"" + trace_path.string() +
      "\",\"receipt\":\"" + receipt_path.string() +
      "\"},\"profile_id\":\"native-observer-cost\","
      "\"qualification_policy\":{\"max_clock_error_ns\":1000},"
      "\"question\":\"native-emitter-overhead\","
      "\"required_categories\":" + categories +
      ",\"required_collectors\":[\"engine-perfetto\"],"
      "\"run_id\":\"observer-cost\","
      "\"schema\":\"dxmt9.trace.request.v1\","
      "\"window\":{\"delay_ms\":0,\"duration_ms\":60000,"
      "\"mode\":\"duration\"}}";
  const std::string digest = sha256(canonical);
  const std::string request =
      "{\"schema\":\"dxmt9.trace.request.v1\","
      "\"run_id\":\"observer-cost\","
      "\"attempt_id\":\"observer-cost\","
      "\"profile_id\":\"native-observer-cost\","
      "\"question\":\"native-emitter-overhead\","
      "\"categories\":" + categories +
      ",\"required_categories\":" + categories +
      ",\"required_collectors\":[\"engine-perfetto\"],"
      "\"optional_collectors\":[],"
      "\"window\":{\"mode\":\"duration\",\"delay_ms\":0,"
      "\"duration_ms\":60000},"
      "\"limits\":{\"max_events\":" + std::to_string(kMaxEvents) +
      ",\"max_bytes\":" + std::to_string(kMaxBytes) +
      ",\"finalize_timeout_ms\":10000},"
      "\"output\":{\"pftrace\":\"" + trace_path.string() +
      "\",\"receipt\":\"" + receipt_path.string() +
      "\"},\"qualification_policy\":{\"max_clock_error_ns\":1000},"
      "\"config_sha256\":\"" + digest + "\"}";
  if (!write_file(request_path, request)) return 3;

  ParsedConfig parsed;
  std::string error;
  if (!parse_request(request_path.string(), parsed, error) ||
      parsed.max_clock_error_ns != 1000 ||
      parsed.max_events != kMaxEvents || parsed.max_bytes != kMaxBytes)
    return 4;

  const auto process_start = std::chrono::steady_clock::now();
  std::array<Measurement, kRepeatCount> off{};
  const bool off_gate = enabled_detail();
  if (off_gate) return 5;
  for (auto& measurement : off)
    measurement = measure_repeat(off_gate, kEventsPerRepeat);

  const TraceConfig config{request_path.c_str(), receipt_path.c_str(),
                           raw_path.c_str(), trace_path.c_str()};
  if (!prepare_and_start(config)) return 6;
  for (uint32_t attempt = 0; attempt < 500 && !enabled_detail(); ++attempt)
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  const bool on_gate = enabled_detail();
  if (!on_gate) return 7;

  std::array<Measurement, kRepeatCount> on{};
  for (auto& measurement : on)
    measurement = measure_repeat(on_gate, kEventsPerRepeat);
  const bool finalized = stop_and_finalize();
  const auto process_end = std::chrono::steady_clock::now();

  std::ifstream receipt_file(receipt_path, std::ios::binary);
  const std::string receipt(std::istreambuf_iterator<char>(receipt_file), {});
  const uint64_t engine_events = parse_receipt_integer(receipt, "engine_events");
  const uint64_t total_loss = parse_receipt_integer(receipt, "total");
  uint64_t off_wall = 0, off_cpu = 0, on_wall = 0, on_cpu = 0;
  uint64_t rejects = 0;
  for (uint32_t i = 0; i < kRepeatCount; ++i) {
    off_wall += off[i].wall_ns;
    off_cpu += off[i].thread_cpu_ns;
    on_wall += on[i].wall_ns;
    on_cpu += on[i].thread_cpu_ns;
    rejects += on[i].rejected_events;
  }
  const uint64_t overhead_cpu_ns_per_event =
      on_cpu > off_cpu ? (on_cpu - off_cpu) /
          (kRepeatCount * kEventsPerRepeat) : 0;
  const uint64_t overhead_wall_ns_per_event =
      on_wall > off_wall ? (on_wall - off_wall) /
          (kRepeatCount * kEventsPerRepeat) : 0;
  const bool budget_passed =
      overhead_cpu_ns_per_event <= kCpuOverheadBudgetNsPerEvent;
  const uint64_t process_elapsed_ns = static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(process_end - process_start)
          .count());

  std::string report =
      "{\"schema\":\"dxmt9.trace.observer_cost.v1\","
      "\"scope\":\"native_emitter_only\","
      "\"event_count_per_repeat\":" + std::to_string(kEventsPerRepeat) +
      ",\"repeat_count\":" + std::to_string(kRepeatCount) +
      ",\"fixed_cpu_overhead_budget_ns_per_event\":" +
      std::to_string(kCpuOverheadBudgetNsPerEvent) +
      ",\"off\":{\"wall_ns_total\":\"" + std::to_string(off_wall) +
      "\",\"thread_cpu_ns_total\":\"" + std::to_string(off_cpu) + "\"},"
      "\"on\":{\"wall_ns_total\":\"" + std::to_string(on_wall) +
      "\",\"thread_cpu_ns_total\":\"" + std::to_string(on_cpu) + "\"},"
      "\"delta\":{\"wall_ns_per_event\":\"" +
      std::to_string(overhead_wall_ns_per_event) +
      "\",\"thread_cpu_ns_per_event\":\"" +
      std::to_string(overhead_cpu_ns_per_event) + "\"},"
      "\"process_elapsed_ns\":\"" + std::to_string(process_elapsed_ns) +
      "\",\"engine_event_count\":\"" + std::to_string(engine_events) +
      "\",\"emit_rejections\":\"" + std::to_string(rejects) +
      "\",\"total_loss\":\"" + std::to_string(total_loss) +
      "\",\"application_checksum\":\"" +
      std::to_string(app_sink.load(std::memory_order_relaxed)) +
      "\",\"budget_passed\":" + (budget_passed ? "true" : "false") +
      ",\"finalized\":" + (finalized ? "true" : "false") + "}\n";
  if (!write_file(report_path, report)) return 8;

  return finalized && rejects == 0 && engine_events ==
      static_cast<uint64_t>(kRepeatCount) * kEventsPerRepeat && total_loss == 0 &&
      budget_passed ? 0 : 9;
}
