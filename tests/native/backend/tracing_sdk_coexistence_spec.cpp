#include "../../../src/dxmt9/dxmt9_trace.hpp"

#include <CommonCrypto/CommonDigest.h>
#include <perfetto.h>

#include <cassert>
#include <chrono>
#include <climits>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <iterator>
#include <memory>
#include <string>
#include <thread>
#include <vector>
#include <unistd.h>

class ExternalSdkDataSource
    : public perfetto::DataSource<ExternalSdkDataSource> {};
PERFETTO_DEFINE_DATA_SOURCE_STATIC_MEMBERS(ExternalSdkDataSource);

namespace {

std::string sha256(const std::string& text) {
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

std::string json_escape(const std::string& text) {
  std::string result;
  for (char ch : text) {
    if (ch == '\\' || ch == '"') result.push_back('\\');
    if (ch == '\n') {
      result += "\\n";
    } else if (ch == '\r') {
      result += "\\r";
    } else {
      result.push_back(ch);
    }
  }
  return result;
}

bool wait_for(std::future<void>& future) {
  return future.wait_for(std::chrono::seconds(5)) ==
      std::future_status::ready;
}

bool read_trace(const std::filesystem::path& path,
                std::vector<char>& bytes) {
  std::ifstream input(path, std::ios::binary);
  if (!input) return false;
  bytes.assign(std::istreambuf_iterator<char>(input), {});
  if (bytes.empty() || bytes.size() > static_cast<size_t>(INT32_MAX)) return false;
  perfetto::protos::pbzero::Trace_Decoder trace(
      reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size());
  auto packets = trace.packet();
  return trace.has_packet() && static_cast<bool>(packets);
}

bool contains_event(const std::vector<char>& bytes,
                    const std::string& name) {
  perfetto::protos::pbzero::Trace_Decoder trace(
      reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size());
  auto packets = trace.packet();
  for (; packets; ++packets) {
    const auto packet_bytes = *packets;
    perfetto::protos::pbzero::TracePacket_Decoder packet(packet_bytes);
    if (!packet.has_track_event()) continue;
    perfetto::protos::pbzero::TrackEvent_Decoder event(packet.track_event());
    if (event.has_name() && event.name().ToStdStringView() == name)
      return true;
  }
  return false;
}

void write_external_event() {
  ExternalSdkDataSource::Trace([](ExternalSdkDataSource::TraceContext context) {
    auto descriptor_packet = context.NewTracePacket();
    auto* descriptor = descriptor_packet->set_track_descriptor();
    descriptor->set_uuid(0x64786d743973646bull);
    descriptor->set_name("external SDK instance");
    descriptor_packet->Finalize();

    auto packet = context.NewTracePacket();
    packet->set_timestamp(static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count()));
    packet->set_timestamp_clock_id(
        perfetto::protos::pbzero::BUILTIN_CLOCK_MONOTONIC);
    auto* event = packet->set_track_event();
    event->set_track_uuid(0x64786d743973646bull);
    event->set_type(perfetto::protos::pbzero::TrackEvent::TYPE_INSTANT);
    event->set_name("external_sdk_probe");
    event->add_categories("sdk_coexistence");
  });
}

bool start_external_session(
    std::unique_ptr<perfetto::TracingSession>& session) {
  perfetto::TracingInitArgs init_args;
  init_args.backends = perfetto::kInProcessBackend;
  perfetto::Tracing::Initialize(init_args);

  perfetto::DataSourceDescriptor descriptor;
  descriptor.set_name("dxmt9_test_external_sdk_source");
  if (!ExternalSdkDataSource::Register(descriptor)) return false;

  perfetto::TraceConfig config;
  config.add_buffers()->set_size_kb(1024);
  config.add_data_sources()->mutable_config()->set_name(
      "dxmt9_test_external_sdk_source");
  session = perfetto::Tracing::NewTrace(perfetto::kInProcessBackend);
  session->Setup(config);
  auto started = std::make_shared<std::promise<void>>();
  auto started_future = started->get_future();
  session->SetOnStartCallback([started] { started->set_value(); });
  session->Start();
  if (!wait_for(started_future)) return false;

  return true;
}

bool finish_external_trace(perfetto::TracingSession& session,
                           const std::filesystem::path& path) {
  if (!session.FlushBlocking(5000)) return false;
  auto stopped = std::make_shared<std::promise<void>>();
  auto stopped_future = stopped->get_future();
  session.SetOnStopCallback([stopped] { stopped->set_value(); });
  session.Stop();
  if (!wait_for(stopped_future)) return false;

  auto output = std::make_shared<std::promise<std::vector<char>>>();
  auto output_future = output->get_future();
  auto accumulated = std::make_shared<std::vector<char>>();
  session.ReadTrace([output, accumulated](auto args) {
    if (args.size)
      accumulated->insert(accumulated->end(), args.data,
                          args.data + args.size);
    if (!args.has_more) output->set_value(std::move(*accumulated));
  });
  if (output_future.wait_for(std::chrono::seconds(5)) !=
      std::future_status::ready)
    return false;
  const std::vector<char> bytes = output_future.get();
  if (bytes.empty()) return false;
  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  return static_cast<bool>(file);
}

std::string facade_request(const std::filesystem::path& trace_path,
                           const std::filesystem::path& receipt_path) {
  const std::string categories = "[\"import\"]";
  const std::string escaped_trace = json_escape(trace_path.string());
  const std::string escaped_receipt = json_escape(receipt_path.string());
  const std::string canonical =
      "{\"attempt_id\":\"coexistence\",\"categories\":" + categories +
      ",\"limits\":{\"finalize_timeout_ms\":5000,\"max_bytes\":16777216,"
      "\"max_events\":128},\"optional_collectors\":[],\"output\":{\"pftrace\":\"" +
      escaped_trace + "\",\"receipt\":\"" + escaped_receipt +
      "\"},\"profile_id\":\"sdk-coexistence\",\"question\":\"coexistence\","
      "\"required_categories\":" + categories +
      ",\"required_collectors\":[\"engine-perfetto\"],\"run_id\":\"coexistence\","
      "\"schema\":\"dxmt9.trace.request.v1\",\"window\":{\"delay_ms\":0,"
      "\"duration_ms\":30000,\"mode\":\"duration\"}}";
  const std::string digest = sha256(canonical);
  return "{\"schema\":\"dxmt9.trace.request.v1\",\"run_id\":\"coexistence\","
      "\"attempt_id\":\"coexistence\",\"profile_id\":\"sdk-coexistence\","
      "\"question\":\"coexistence\",\"categories\":" + categories +
      ",\"required_categories\":" + categories +
      ",\"required_collectors\":[\"engine-perfetto\"],"
      "\"optional_collectors\":[],\"window\":{\"mode\":\"duration\","
      "\"delay_ms\":0,\"duration_ms\":30000},\"limits\":{\"max_events\":128,"
      "\"max_bytes\":16777216,\"finalize_timeout_ms\":5000},\"output\":{\"pftrace\":\"" +
      escaped_trace + "\",\"receipt\":\"" + escaped_receipt +
      "\"},\"config_sha256\":\"" + digest + "\"}";
}

}  // namespace

int main(int argc, char** argv) {
  using namespace dxmt9::tracing;
  if (argc > 2) return 2;
  unsetenv("DXMT9_TRACE_CONFIG");
  const bool preserve = argc == 2;
  const std::filesystem::path root = preserve
      ? std::filesystem::absolute(argv[1])
      : std::filesystem::temp_directory_path();
  const auto output_dir = root /
      ("dxmt9-tracing-sdk-coexistence-" + std::to_string(getpid()));
  std::filesystem::create_directories(output_dir);
  const auto external_trace_path = output_dir / "external-sdk.pftrace";
  const auto facade_trace_path = output_dir / "facade.pftrace";
  const auto receipt_path = output_dir / "facade-receipt.json";
  const auto raw_path = output_dir / "facade-raw.jsonl";
  const auto request_path = output_dir / "facade-request.json";

  // This process directly links the pinned SDK archive. dxmt9_dep supplies
  // the facade and provider shared objects, each with its own hidden SDK copy.
  std::unique_ptr<perfetto::TracingSession> external_session;
  assert(start_external_session(external_session));

  const std::string request = facade_request(facade_trace_path, receipt_path);
  {
    std::ofstream file(request_path, std::ios::binary | std::ios::trunc);
    file << request;
    assert(static_cast<bool>(file));
  }
  TraceConfig config{request_path.c_str(), receipt_path.c_str(),
                     raw_path.c_str(), facade_trace_path.c_str()};
  assert(prepare_and_start(config));
  for (unsigned attempt = 0; attempt < 500 && !enabled(); ++attempt)
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  assert(enabled());
  write_external_event();
  TraceEvent event{};
  event.timestamp_ns = now_ns();
  event.process_id = static_cast<uint64_t>(getpid());
  event.thread_id = native_thread_id();
  event.clock_id = 3;
  event.event_id = static_cast<uint16_t>(EventId::ImportBegin);
  event.category = static_cast<uint8_t>(Category::Import);
  event.phase = static_cast<uint8_t>(Phase::Instant);
  assert(emit(event));
  assert(stop_and_finalize());
  assert(finish_external_trace(*external_session, external_trace_path));

  std::vector<char> external_trace;
  std::vector<char> facade_trace;
  assert(read_trace(external_trace_path, external_trace));
  assert(read_trace(facade_trace_path, facade_trace));
  assert(contains_event(external_trace, "external_sdk_probe"));
  assert(contains_event(facade_trace, "import_begin"));
  assert(external_trace_path != facade_trace_path);
  assert(std::filesystem::file_size(external_trace_path) > 0);
  assert(std::filesystem::file_size(facade_trace_path) > 0);
  assert(std::filesystem::file_size(raw_path) > 0);
  std::ifstream receipt_file(receipt_path, std::ios::binary);
  const std::string receipt_text(std::istreambuf_iterator<char>(receipt_file), {});
  assert(receipt_text.find("\"status\":\"complete\"") !=
         std::string::npos);
  if (!preserve) std::filesystem::remove_all(output_dir);
  std::cout << "Independent Perfetto SDK and dxmt9 facade captures passed\n";
}
