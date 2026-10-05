#include "render_tape_cpu_v4_admission.hpp"
#include "device_c_render_tape_capture.hpp"
#include "device_c_render_tape_runtime_objects.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <vector>

// The trusted launcher authenticates the original bundle before supplying
// unchanged events and blob paths. This diagnostic independently hashes bytes
// and runs the existing pure General/input gates. It has no replay or queue
// operation and accepts no execution, reduction, or publication-policy option.
namespace {

using namespace dxmt9::d3d9;
namespace runtime = dxmt9::d3d9::render_tape_runtime;
namespace fs = std::filesystem;
constexpr std::uint64_t eventsByteLimit = 512ull * 1024ull * 1024ull;
constexpr std::uint64_t aggregateBlobByteLimit = 512ull * 1024ull * 1024ull;
constexpr std::size_t blobCountLimit = 8192u;
constexpr auto noIndex = runtime::kGeneralCpuNoIndex;

void require(bool condition, const char* reason) {
  if (!condition) throw std::runtime_error(reason);
}

struct Options {
  fs::path events;
  std::vector<fs::path> blobs;
};

Options arguments(int argc, char** argv) {
  Options options;
  bool eventsSeen = false;
  for (int index = 1; index < argc; ++index) {
    const std::string_view option(argv[index]);
    require(option == "--events" || option == "--blob", "unknown-option");
    require(index + 1 < argc, "missing-option-value");
    const std::string_view value(argv[++index]);
    require(!value.empty(), "empty-input-path");
    if (option == "--events") {
      require(!eventsSeen, "duplicate-events-option");
      eventsSeen = true;
      options.events = value;
    } else {
      require(options.blobs.size() < blobCountLimit, "blob-count-limit");
      options.blobs.emplace_back(value);
    }
  }
  require(eventsSeen, "events-option-required");
  return options;
}

std::vector<std::byte> readBoundedFile(const fs::path& path, std::uint64_t limit) {
  std::error_code error;
  require(fs::is_regular_file(path, error) && !error, "input-not-regular-file");
  std::ifstream stream(path, std::ios::binary | std::ios::ate);
  require(static_cast<bool>(stream), "input-open-failed");
  const auto end = stream.tellg();
  require(end >= 0, "input-size-unavailable");
  const auto size = static_cast<std::uint64_t>(end);
  require(size <= limit && size <= std::numeric_limits<std::size_t>::max() &&
      size <= static_cast<std::uint64_t>(std::numeric_limits<std::streamsize>::max()),
      "input-byte-limit");
  std::vector<std::byte> bytes(static_cast<std::size_t>(size));
  stream.seekg(0);
  require(static_cast<bool>(stream), "input-seek-failed");
  if (!bytes.empty())
    require(static_cast<bool>(stream.read(reinterpret_cast<char*>(bytes.data()),
        static_cast<std::streamsize>(bytes.size()))), "input-read-failed");
  require(stream.peek() == std::char_traits<char>::eof() && !stream.bad(), "input-size-changed");
  return bytes;
}

struct Evidence {
  std::uint64_t eventsBytes = 0u;
  RenderTapeDigest eventsDigest{};
  bool eventsLoaded = false;
  std::size_t requestedBlobs = 0u;
  std::size_t loadedBlobs = 0u;
  std::uint64_t blobBytes = 0u;
  bool generalAttempted = false;
  FrameTapeReplayResult general{};
  std::optional<dxmt9::test::ControlledCpuV4InputAdmission> v4;
  std::optional<runtime::GeneralCpuAdmissionResult> legacy;
};

void quoted(std::string_view value) {
  constexpr char hex[] = "0123456789abcdef";
  std::cout << '"';
  for (const unsigned char byte : value) {
    if (byte == '"' || byte == '\\') std::cout << '\\' << static_cast<char>(byte);
    else if (byte < 0x20u)
      std::cout << "\\u00" << hex[byte >> 4u] << hex[byte & 15u];
    else std::cout << static_cast<char>(byte);
  }
  std::cout << '"';
}

void index(std::uint32_t value) {
  if (value == noIndex) std::cout << "null";
  else std::cout << value;
}

void digest(const RenderTapeDigest& value) {
  constexpr char hex[] = "0123456789abcdef";
  std::cout << '"';
  for (const auto byte : value) {
    const auto number = std::to_integer<unsigned>(byte);
    std::cout << hex[number >> 4u] << hex[number & 15u];
  }
  std::cout << '"';
}

void counts(const runtime::GeneralCpuAdmissionCounts& value) {
  std::cout << "{\"events\":" << value.events << ",\"objects\":" << value.objects
      << ",\"mutations\":" << value.mutations << ",\"chunks\":" << value.chunks
      << ",\"records\":" << value.records << ",\"draws\":" << value.draws
      << ",\"blobs\":" << value.blobs << ",\"blob_bytes\":" << value.blobBytes
      << ",\"bootstrap_chunks\":" << value.bootstrapChunks
      << ",\"bootstrap_records\":" << value.bootstrapRecords
      << ",\"presents\":" << value.presents
      << ",\"present_completions\":" << value.presentCompletions
      << ",\"query_definitions\":" << value.queryDefinitions
      << ",\"query_issues\":" << value.queryIssues
      << ",\"query_polls\":" << value.queryPolls
      << ",\"query_checkpoints\":" << value.queryCheckpoints << '}';
}

void admission(bool attempted, bool accepted, runtime::GeneralCpuAdmissionReason reason,
    std::uint32_t event, std::uint32_t chunk, std::uint32_t record,
    const runtime::GeneralCpuAdmissionCounts* admittedCounts) {
  std::cout << "\"attempted\":" << (attempted ? "true" : "false")
      << ",\"status\":";
  quoted(attempted ? accepted ? "accepted" : "rejected" : "not-run");
  std::cout << ",\"accepted\":" << (attempted && accepted ? "true" : "false") << ",\"reason\":";
  quoted(attempted ? runtime::generalCpuAdmissionReasonName(reason) : "not-run");
  std::cout << ",\"event\":"; index(event);
  std::cout << ",\"chunk\":"; index(chunk);
  std::cout << ",\"record\":"; index(record);
  std::cout << ",\"counts\":";
  if (admittedCounts) counts(*admittedCounts);
  else std::cout << "null";
}

void emit(const Evidence& evidence, std::string_view failure) {
  const bool accepted = failure.empty() && evidence.general.complete() &&
      evidence.v4 && evidence.v4->accepted();
  std::cout << "{\"schema\":\"dxmt9.render_tape.cpu_v4_input_diagnostic.v1\","
      "\"input_only\":true,\"execution_authorized\":false,\"success\":"
      << (accepted ? "true" : "false") << ",\"failure\":";
  if (failure.empty()) std::cout << "null";
  else quoted(failure);
  std::cout << ",\"files\":{\"events_loaded\":" << (evidence.eventsLoaded ? "true" : "false")
      << ",\"events_bytes\":" << evidence.eventsBytes << ",\"events_sha256\":";
  if (evidence.eventsLoaded) digest(evidence.eventsDigest);
  else std::cout << "null";
  std::cout << ",\"blobs_requested\":" << evidence.requestedBlobs
      << ",\"blobs_loaded\":" << evidence.loadedBlobs << ",\"blob_bytes\":" << evidence.blobBytes
      << "},\"general_preflight\":{\"attempted\":" << (evidence.generalAttempted ? "true" : "false")
      << ",\"status\":";
  quoted(evidence.generalAttempted ? frameTapeReplayStatusName(evidence.general.status) : "not-run");
  std::cout << ",\"valid\":" << (evidence.generalAttempted && evidence.general.complete() ? "true" : "false")
      << ",\"event\":";
  index(evidence.generalAttempted ? evidence.general.failedEventIndex : noIndex);
  const auto& coverage = evidence.general.coverage;
  std::cout << ",\"counts\":{\"events\":" << coverage.eventCount
      << ",\"object_definitions\":" << coverage.objectDefinitions
      << ",\"mutations\":" << coverage.seedMutations
      << ",\"bootstrap_chunks\":" << coverage.bootstrapChunks
      << ",\"command_chunks\":" << coverage.commandChunks
      << ",\"command_records\":" << coverage.commandRecords
      << ",\"draw_primitive\":" << coverage.drawPrimitiveRecords
      << ",\"draw_indexed_primitive\":" << coverage.drawIndexedPrimitiveRecords
      << ",\"draw_primitive_up\":" << coverage.drawPrimitiveUpRecords
      << "}},\"v4_input\":{\"profile\":\"raw256\","
      "\"body_records_per_chunk_limit\":256,\"bootstrap_records_per_chunk_limit\":64,"
      "\"source_progress_requirement\":\"required-unproven\","
      "\"queue_authorized\":false,\"semantic_authorized\":false,\"gpu_authorized\":false,";
  if (evidence.v4) {
    const auto& v4 = *evidence.v4;
    admission(true, v4.accepted(), v4.reason(), v4.eventIndex(), v4.chunkIndex(), v4.recordIndex(), &v4.counts());
  } else admission(false, false, runtime::GeneralCpuAdmissionReason::Accepted, noIndex, noIndex, noIndex, nullptr);
  std::cout << "},\"legacy_extended64\":{\"body_records_per_chunk_limit\":64,";
  if (evidence.legacy) {
    const auto& legacy = *evidence.legacy;
    admission(true, legacy.accepted(), legacy.reason, legacy.eventIndex, legacy.chunkIndex,
        legacy.recordIndex, &legacy.counts);
  } else admission(false, false, runtime::GeneralCpuAdmissionReason::Accepted, noIndex, noIndex, noIndex, nullptr);
  std::cout << "}}\n";
}

int run(int argc, char** argv, Evidence& evidence) {
  const auto options = arguments(argc, argv);
  evidence.requestedBlobs = options.blobs.size();
  auto events = readBoundedFile(options.events, eventsByteLimit);
  evidence.eventsBytes = events.size();
  evidence.eventsDigest = RenderTapeCaptureSession::sha256(events);
  evidence.eventsLoaded = true;
  std::vector<std::vector<std::byte>> blobStorage;
  std::vector<RenderTapeProviderBlob> blobs;
  blobStorage.reserve(options.blobs.size());
  blobs.reserve(options.blobs.size());
  for (const auto& path : options.blobs) {
    blobStorage.push_back(readBoundedFile(path, aggregateBlobByteLimit - evidence.blobBytes));
    const auto& bytes = blobStorage.back();
    const auto hash = RenderTapeCaptureSession::sha256(bytes);
    evidence.blobBytes += bytes.size();
    ++evidence.loadedBlobs;
    blobs.push_back({hash, bytes});
  }
  runtime::GeneralPreflightPlan plan;
  evidence.generalAttempted = true;
  evidence.general = runtime::buildGeneralReplayPlan(events, blobs, plan);
  if (!evidence.general.complete()) {
    emit(evidence, "general-preflight-rejected");
    return 1;
  }
  evidence.v4 = dxmt9::test::admitControlledCpuV4Input(plan);
  evidence.legacy = runtime::admitGeneralCpuPlan(plan, runtime::generalCpuExtendedInputCaps());
  if (!evidence.v4->accepted()) {
    emit(evidence, "controlled-cpu-v4-input-rejected");
    return 1;
  }
  emit(evidence, {});
  return 0;
}

} // namespace

int main(int argc, char** argv) {
  Evidence evidence;
  try { return run(argc, argv, evidence); }
  catch (const std::exception& error) {
    emit(evidence, error.what());
    return 1;
  } catch (...) {
    emit(evidence, "unexpected-input-diagnostic-failure");
    return 1;
  }
}
