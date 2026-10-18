#include "../../../src/dxmt9/dxmt9_trace.hpp"
#include "../../../src/dxmt9/dxmt9_trace_bindings.hpp"
#include "../../../src/dxmt9/tracing/config.hpp"
#include "../../../src/dxmt9/observations/snapshot_sink.hpp"

#include <CommonCrypto/CommonDigest.h>

#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <unistd.h>

#ifndef DXMT9_TEST_PERFETTO
#define DXMT9_TEST_PERFETTO 0
#endif

namespace {

using namespace dxmt9::tracing;

std::string sha256(const std::string& text) {
  unsigned char digest[CC_SHA256_DIGEST_LENGTH];
  CC_SHA256(text.data(), static_cast<CC_LONG>(text.size()), digest);
  static constexpr char hex[] = "0123456789abcdef";
  std::string result;
  result.reserve(sizeof(digest) * 2);
  for (unsigned char byte : digest) {
    result.push_back(hex[byte >> 4]);
    result.push_back(hex[byte & 15]);
  }
  return result;
}

struct Capture {
  std::filesystem::path directory;
  std::filesystem::path request_path;
  std::filesystem::path trace_path;
  std::filesystem::path receipt_path;
  std::filesystem::path raw_path;

  Capture(const std::filesystem::path& root, const std::string& name,
          uint32_t max_events = 64, bool include_policy = true,
          uint32_t delay_ms = 0, bool optional_metal_timing = false)
      : directory(root / name), include_policy_(include_policy),
        optional_metal_timing_(optional_metal_timing) {
    std::filesystem::create_directories(directory);
    request_path = directory / "request.json";
    trace_path = directory / "trace.pftrace";
    receipt_path = directory / "receipt.json";
    raw_path = directory / "trace.pftrace.raw.jsonl";
    writeRequest(max_events, delay_ms);
  }

  TraceConfig config() const noexcept {
    return {request_path.c_str(), receipt_path.c_str(), raw_path.c_str(),
            trace_path.c_str()};
  }

  std::string readRaw() const {
    std::ifstream input(raw_path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), {}};
  }

  std::string readReceipt() const {
    std::ifstream input(receipt_path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), {}};
  }

private:
  bool include_policy_;
  bool optional_metal_timing_;

  void writeRequest(uint32_t max_events, uint32_t delay_ms) {
    constexpr char categories[] = "[\"replay\",\"wait\",\"encode\"]";
    const std::string optional_collectors = optional_metal_timing_
        ? "[\"metal-timing\"]" : "[]";
    const std::string policy = include_policy_
        ? "\"qualification_policy\":{\"max_clock_error_ns\":1000},"
        : "";
    const std::string canonical =
        "{\"attempt_id\":\"attempt-" + directory.filename().string() +
        "\",\"categories\":[\"replay\",\"wait\",\"encode\"],\"limits\":{\"finalize_timeout_ms\":5000,\"max_bytes\":16777216,\"max_events\":" +
        std::to_string(max_events) +
        "},\"optional_collectors\":" + optional_collectors + ",\"output\":{\"pftrace\":\"" +
        trace_path.string() + "\",\"receipt\":\"" + receipt_path.string() +
        "\"},\"profile_id\":\"profile\"," + policy +
        "\"question\":\"session-spec\","
        "\"required_categories\":[\"replay\",\"wait\",\"encode\"],\"required_collectors\":[\"engine-perfetto\"],"
        "\"run_id\":\"run-" + directory.filename().string() +
        "\",\"schema\":\"dxmt9.trace.request.v1\",\"window\":{\"delay_ms\":" +
        std::to_string(delay_ms) + ",\"duration_ms\":30000,\"mode\":\"duration\"}}";
    const std::string digest = sha256(canonical);
    const std::string request =
        "{\"schema\":\"dxmt9.trace.request.v1\",\"run_id\":\"run-" +
        directory.filename().string() + "\",\"attempt_id\":\"attempt-" +
        directory.filename().string() +
        "\",\"profile_id\":\"profile\",\"question\":\"session-spec\"," +
        policy + "\"categories\":" +
        categories + ",\"required_categories\":" + categories +
        ",\"required_collectors\":[\"engine-perfetto\"],\"optional_collectors\":" +
        optional_collectors + ","
        "\"window\":{\"mode\":\"duration\",\"delay_ms\":" +
        std::to_string(delay_ms) + ",\"duration_ms\":30000},"
        "\"limits\":{\"max_events\":" + std::to_string(max_events) +
        ",\"max_bytes\":16777216,\"finalize_timeout_ms\":5000},\"output\":{\"pftrace\":\"" +
        trace_path.string() + "\",\"receipt\":\"" + receipt_path.string() +
        "\"},\"config_sha256\":\"" + digest + "\"}";
    std::ofstream output(request_path, std::ios::binary);
    output << request;
    assert(output.good());
  }
};

TraceEvent replayEvent(uint16_t event_id =
                           static_cast<uint16_t>(EventId::ReplayPublish)) {
  TraceEvent event{};
  event.timestamp_ns = now_ns();
  event.work_id = 17;
  event.clock_id = 3;
  event.event_id = event_id;
  event.category = static_cast<uint8_t>(Category::Replay);
  event.phase = static_cast<uint8_t>(Phase::Instant);
  return event;
}

// An incomplete stop reports the decision before its retained owner finishes
// publishing the terminal proof. Rearm is the production quiescence gate;
// retry only this expected cold lifecycle rejection, with a bounded deadline.
[[maybe_unused]] bool prepareAfterOwnerRelease(const Capture& capture) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  do {
    if (prepare_and_start(capture.config())) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  } while (std::chrono::steady_clock::now() < deadline);
  return false;
}

void waitForCollection() {

  for (unsigned i = 0; i < 1000 && !enabled(); ++i)
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  assert(enabled());
}

}  // namespace

int main() {
  using namespace dxmt9::tracing;
  unsetenv("DXMT9_TRACE_CONFIG");
  const auto root = std::filesystem::temp_directory_path() /
      ("dxmt9-tracing-session-" + std::to_string(getpid()));
  std::filesystem::remove_all(root);
  std::filesystem::create_directories(root);

  Capture malformed(root, "malformed");
  {
    std::ofstream output(malformed.request_path, std::ios::binary | std::ios::trunc);
    output << R"({"schema":)";
  }
  assert(!prepare_and_start(malformed.config()));
  assert(!enabled());

  Capture defaultPolicy(root, "default-policy", 64, false);
  ParsedConfig parsedPolicy{};
  std::string parseError;
  assert(parse_request(defaultPolicy.request_path.string(), parsedPolicy,
                       parseError));
  assert(parsedPolicy.max_clock_error_ns == 1000);
  Capture explicitPolicy(root, "explicit-policy");
  ParsedConfig parsedExplicitPolicy{};
  assert(parse_request(explicitPolicy.request_path.string(),
                       parsedExplicitPolicy, parseError));
  assert(parsedExplicitPolicy.max_clock_error_ns == 1000);

  // Owner registrations while the atomic generation gate is disabled must not
  // consume the permanently non-reused bounded registry slots.
  std::array<int, 300> inactiveOwners{};
  for (const int& owner : inactiveOwners) register_owner(&owner);
  assert(owner_id(&inactiveOwners.front()) == 0);

#if DXMT9_TEST_PERFETTO
  // Re-arm on one producer thread. Its thread-local slot cache must be
  // generation-qualified and the output paths must remain disjoint.
  Capture first(root, "generation-one");
  Capture second(root, "generation-two");
  assert(prepare_and_start(first.config()));
  waitForCollection();
  dxmt9::observations::SnapshotSink oldSnapshot(dxmt9::observations::Source::QueryBackend, 2, 64);
  assert(oldSnapshot.value(0, 123)); // Staged, no native rows before the final field.
  dxmt9::observations::SnapshotSink oldInvalid(dxmt9::observations::Source::QueryBackend, 2, 64);
  assert(emit(replayEvent()));
  assert(stop_and_finalize());
  assert(!emit(replayEvent()));
  const std::string firstReceipt = first.readReceipt();
  assert(firstReceipt.find("\"status\":\"complete\"") !=
         std::string::npos);
  assert(firstReceipt.find("\"engine-perfetto\":{\"status\":\"complete\"") !=
         std::string::npos);
  assert(firstReceipt.find("\"metal-timing\":{\"status\":\"unavailable\"") !=
         std::string::npos);

  assert(prepare_and_start(second.config()));
  waitForCollection();
  bool admitted = true;
  for (std::uint16_t i = 1; i < std::size(dxmt9::observations::backend_fields); ++i)
    admitted = oldSnapshot.value(i, 456);
  assert(!admitted);
  assert(!oldInvalid.value(0, UINT64_MAX));
  assert(emit(replayEvent()));
  assert(stop_and_finalize());
  const std::string firstRaw = first.readRaw();
  const std::string secondRaw = second.readRaw();
  assert(firstRaw.find("\"max_clock_error_ns\":\"1000\"") !=
         std::string::npos);
  assert(firstRaw.find("\"thread_id\":\"4294967297\"") != std::string::npos);
  assert(secondRaw.find("\"thread_id\":\"8589934593\"") != std::string::npos);

  // Requested optional timing is unavailable until a calibrated GPU sample
  // pair is appended. A normal engine event cannot make it appear complete.
  Capture noGpuSamples(root, "optional-metal-timing", 64, true, 0, true);
  assert(prepare_and_start(noGpuSamples.config()));
  waitForCollection();
  assert(emit(replayEvent()));
  assert(stop_and_finalize());
  const std::string noGpuReceipt = noGpuSamples.readReceipt();
  assert(noGpuReceipt.find("\"metal-timing\":{\"status\":\"unavailable\"") !=
         std::string::npos);
  assert(noGpuReceipt.find("\"optional_collectors\":{\"metal-timing\":{\"status\":\"unavailable\"") !=
         std::string::npos);

  // An owner from generation N unregistering after N+1 arms must not close
  // N+1 or disable its owner registration gate.
  Capture ownerOld(root, "owner-old-generation");
  Capture ownerNew(root, "owner-new-generation");
  int oldOwner = 0;
  int newOwner = 0;
  assert(prepare_and_start(ownerOld.config()));
  waitForCollection();
  register_owner(&oldOwner);
  assert(owner_id(&oldOwner) != 0);
  assert(stop_and_finalize());
  assert(prepare_and_start(ownerNew.config()));
  waitForCollection();
  unregister_owner(&oldOwner);
  register_owner(&newOwner);
  assert(owner_id(&newOwner) != 0);
  assert(session_configured());
  assert(emit(replayEvent()));
  assert(stop_and_finalize());

  // Concurrent environment initialization must converge on the same armed
  // session instead of reporting false while a peer prepares it.
  Capture initRace(root, "concurrent-initialize");
  assert(setenv("DXMT9_TRACE_CONFIG", initRace.request_path.c_str(), 1) == 0);
  std::array<bool, 8> initResults{};
  std::array<std::thread, 8> initializers;
  for (size_t i = 0; i < initializers.size(); ++i) {
    initializers[i] = std::thread(
        [&, i] { initResults[i] = initialize_from_environment(); });
  }
  for (auto& initializer : initializers) initializer.join();
  for (bool initialized : initResults) assert(initialized);
  waitForCollection();
  assert(emit(replayEvent()));
  assert(stop_and_finalize());
  unsetenv("DXMT9_TRACE_CONFIG");

  // Destroying a scope from the old generation after a new generation starts
  // must not append an end event to the new capture.
  Capture scoped(root, "old-scope");
  Capture afterScope(root, "after-old-scope");
  assert(prepare_and_start(scoped.config()));
  waitForCollection();
  auto oldScope = std::make_unique<TraceScope>(
      static_cast<uint8_t>(Category::Replay),
      static_cast<uint16_t>(EventId::ReplayBegin),
      static_cast<uint16_t>(EventId::ReplayEnd), 0, 19);
  assert(stop_and_finalize());
  assert(prepare_and_start(afterScope.config()));
  waitForCollection();
  assert(emit(replayEvent()));
  oldScope.reset();
  assert(stop_and_finalize());
  assert(afterScope.readRaw().find("\"name\":\"replay_end\"") ==
         std::string::npos);

  // Bridge-facing delayed callbacks and teardown are generation-qualified.
  Capture delayedOld(root, "delayed-old-generation");
  Capture delayedNew(root, "delayed-new-generation");
  assert(prepare_and_start(delayedOld.config()));
  waitForCollection();
  const uint64_t oldGeneration = capture_generation();
  assert(oldGeneration != 0);
  assert(stop_and_finalize());
  assert(prepare_and_start(delayedNew.config()));
  waitForCollection();
  const TraceEvent delayedEvent = replayEvent();
  assert(!emit_batch_captured(
      std::span<const TraceEvent>(&delayedEvent, 1), oldGeneration, 1));
  assert(!stop_and_finalize(oldGeneration));
  assert(enabled());
  assert(emit(replayEvent()));
  assert(stop_and_finalize());
  assert(delayedNew.readRaw().find("\"loss\":{\"total\":\"0\"}") !=
         std::string::npos);

  // Exactly one concurrent caller owns finalization; both callers may return
  // without racing a second join or republishing the receipt.
  Capture concurrent(root, "concurrent-stop");
  assert(prepare_and_start(concurrent.config()));
  waitForCollection();
  assert(emit(replayEvent()));
  std::array<bool, 2> results{};
  std::array<std::thread, 2> stoppers{
      std::thread([&] { results[0] = stop_and_finalize(); }),
      std::thread([&] { results[1] = stop_and_finalize(); })};
  for (auto& stopper : stoppers) stopper.join();
  assert(results[0] || results[1]);
  assert(concurrent.readReceipt().find("\"status\":\"complete\"") !=
         std::string::npos);

  // Stopping during the armed delay seals an explicitly incomplete capture.
  Capture immediate(root, "immediate-stop", 64, true, 5000);
  assert(prepare_and_start(immediate.config()));
  assert(!stop_and_finalize());
  assert(immediate.readReceipt().find("\"status\":\"incomplete\"") !=
         std::string::npos);

  // Batch loss is admitted under the same generation writer lease as events.
  Capture lostBatch(root, "admitted-batch-loss");
  assert(prepareAfterOwnerRelease(lostBatch));
  waitForCollection();
  const TraceEvent lostEvent = replayEvent();
  assert(emit_batch(std::span<const TraceEvent>(&lostEvent, 1), 1));
  assert(!stop_and_finalize());
  assert(lostBatch.readRaw().find("\"loss\":{\"total\":\"1\"}") !=
         std::string::npos);

  // A rearmed session starts with its own loss count.
  Capture afterLoss(root, "after-admitted-batch-loss");
  assert(prepareAfterOwnerRelease(afterLoss));
  waitForCollection();
  assert(emit(replayEvent()));
  assert(stop_and_finalize());
  assert(afterLoss.readRaw().find("\"loss\":{\"total\":\"0\"}") !=
         std::string::npos);

  Capture numericOverflow(root, "snapshot-numeric-overflow");
  assert(prepare_and_start(numericOverflow.config()));
  waitForCollection();
  dxmt9::observations::SnapshotSink numeric(dxmt9::observations::Source::PerfFrame, 1, 0);
  assert(!numeric.value(0, UINT64_MAX));
  assert(emit(replayEvent()));
  assert(!stop_and_finalize());
  assert(numericOverflow.readRaw().find("\"loss\":{\"total\":\"1\"}") != std::string::npos);

  Capture overflow(root, "overflow", 1);
  assert(prepareAfterOwnerRelease(overflow));
  waitForCollection();
  assert(emit(replayEvent()));
  dxmt9::observations::SnapshotSink exhausted(dxmt9::observations::Source::QueryBackend, 1, 0);
  for (std::uint16_t i = 0; i < std::size(dxmt9::observations::backend_fields); ++i)
    admitted = exhausted.value(i, 1);
  assert(!admitted);
  assert(!emit(replayEvent()));
  assert(!stop_and_finalize());
  assert(overflow.readReceipt().find("\"status\":\"incomplete\"") !=
         std::string::npos);
  // The final incomplete capture may still own proof I/O. A subsequent clean
  // capture proves owner release before this fixture removes its artifacts.
  Capture afterOverflow(root, "after-overflow-owner-release");
  assert(prepareAfterOwnerRelease(afterOverflow));
  waitForCollection();
  assert(emit(replayEvent()));
  assert(stop_and_finalize());
#else
  Capture inactive(root, "sdk-off");
  assert(!prepare_and_start(inactive.config()));
  assert(!enabled());
  assert(!stop_and_finalize());
#endif

  std::filesystem::remove_all(root);
  std::cout << "Tracing session lifecycle integration passed\n";
}
