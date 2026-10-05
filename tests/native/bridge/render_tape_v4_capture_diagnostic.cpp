#include "device_c_common.hpp"
#include "device_c_render_tape_capture.hpp"
#include "device_c_render_tape_action_profile_component.hpp"
#include "device_c_render_tape_provider.hpp"
#include "device_c_render_tape_runtime_objects.hpp"
#include "dxmt9/dxmt9_device.hpp"
#include "dxmt9/dxmt9_draw_encoder.hpp"
#include "dxmt9/dxmt9_perf_counters.hpp"
#include "../backend/render_tape_encode_destination.hpp"
#include "../backend/render_tape_encode_draw_witness.hpp"
#include "../backend/render_tape_encode_original_adapter.hpp"
#include "../backend/render_tape_v4_capacity_policy.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <span>
#include <streambuf>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

namespace dxmt9 {

// Install once on the cold fresh-device boundary. Never detach while an
// EncodeContext may retain the recorder: its owner outlives device teardown.
struct RenderTapeEncodeDrawWitnessAccess {
  static const char* install(CommandQueue& queue, encoders::EncodeDrawRecorder& recorder) noexcept {
    try {
      std::lock_guard lock(queue.mutex_);
      if (queue.renderPartitionConfig_.resolved == render::PartitionExecutionMode::ExplicitParallel)
        return "encode-draw-witness-parallel-domain-unsupported";
      if (queue.testOnlyDrawRecorder_) return "encode-draw-witness-recorder-already-installed";
      queue.testOnlyDrawRecorder_ = &recorder;
      return nullptr;
    } catch (...) { return "encode-draw-witness-install-failed"; }
  }
};

} // namespace dxmt9

// Private, single-run native qualifier. The launcher owns manifest validation
// through load_bundle(allow_identity_v4=True), a wall deadline, and receipts.
// This executable reauthenticates owned component bytes and the production
// pair/General plan. It does not open any public V4 execution/policy gate.
namespace {

using namespace dxmt9::d3d9;
using GeneralPlan = render_tape_runtime::GeneralPreflightPlan;
using Counters = dxmt9::perf::RenderTapeReplaySnapshot;
namespace fs = std::filesystem;
constexpr std::uint64_t componentLimit = 64u * 1024u * 1024u;
constexpr std::uint64_t blobLimit = kRenderTapeHardMaxBlobBytes;
// Match the launcher total-image budget, rather than the smaller tape
// component cap: debug native libraries can be larger than 64 MiB.
constexpr std::uint64_t loadedImageByteLimit = 1024u * 1024u * 1024u;
constexpr std::size_t loadedImageCountLimit = 256u;
constexpr std::uint64_t repeatedReadbackByteLimit = 512u * 1024u * 1024u;
constexpr std::uint64_t iterationJsonByteLimit = 16u * 1024u * 1024u;
constexpr std::uint64_t repeatedJsonByteLimit = 128u * 1024u * 1024u;
constexpr auto noIndex = UINT32_MAX;

void require(bool value, const char* reason) {
  if (!value) throw std::runtime_error(reason);
}

std::string quoted(std::string_view value) {
  constexpr char hex[] = "0123456789abcdef";
  std::string result = "\"";
  for (const unsigned char byte : value) {
    if (byte == '"' || byte == '\\') {
      result.push_back('\\');
      result.push_back(static_cast<char>(byte));
    } else if (byte < 0x20u) {
      result += "\\u00";
      result.push_back(hex[byte >> 4u]);
      result.push_back(hex[byte & 15u]);
    } else result.push_back(static_cast<char>(byte));
  }
  return result + '"';
}

std::string hexBytes(std::span<const std::byte> bytes) {
  constexpr char hex[] = "0123456789abcdef";
  std::string result;
  result.reserve(bytes.size() * 2u);
  for (const auto value : bytes) {
    const auto byte = std::to_integer<unsigned>(value);
    result.push_back(hex[byte >> 4u]);
    result.push_back(hex[byte & 15u]);
  }
  return result;
}

RenderTapeDigest parseDigest(std::string_view text) {
  require(text.size() == 64u, "digest-must-be-64-lowercase-hex");
  const auto nibble = [](char value) -> unsigned {
    if (value >= '0' && value <= '9') return static_cast<unsigned>(value - '0');
    if (value >= 'a' && value <= 'f') return static_cast<unsigned>(value - 'a') + 10u;
    return 16u;
  };
  RenderTapeDigest result{};
  for (std::size_t index = 0u; index < result.size(); ++index) {
    const auto high = nibble(text[index * 2u]), low = nibble(text[index * 2u + 1u]);
    require(high < 16u && low < 16u, "digest-must-be-64-lowercase-hex");
    result[index] = static_cast<std::byte>((high << 4u) | low);
  }
  require(std::any_of(result.begin(), result.end(), [](auto byte) {
    return byte != std::byte{};
  }), "digest-must-be-nonzero");
  return result;
}

struct Options {
  fs::path bundle{};
  fs::path readbackDirectory{};
  bool inspect = false;
  bool observeEncodeDraws = false;
  bool repeated = false;
  std::size_t warmup = 0u, repeat = 1u;
  std::string resetPolicy{};
  std::string route{};
  std::string producerContract = "none";
  std::string publicationMode = "exact-source-diagnostic";
  std::size_t drawCommandLimit = 0u;
  dxmt9::test::RenderTapeV4CapacityPolicy capacityPolicy{};
  RenderTapeDigest events{}, identity{}, topology{}, source{}, output{}, plan{}, actionProfile{};
};

Options arguments(int argc, char** argv) {
  Options options;
  std::set<std::string> seen;
  for (int index = 1; index < argc; ++index) {
    const std::string option = argv[index];
    require(seen.insert(option).second, "duplicate-option");
    if (option == "--inspect-plan") { options.inspect = true; continue; }
    if (option == "--observe-encode-draws") { options.observeEncodeDraws = true; continue; }
    require(index + 1 < argc, "missing-option-value");
    const std::string value = argv[++index];
    if (option == "--bundle") options.bundle = value;
    else if (option == "--readback-dir") options.readbackDirectory = value;
    else if (option == "--replay-route") options.route = value;
    else if (option == "--producer-contract") options.producerContract = value;
    else if (option == "--publication-mode") options.publicationMode = value;
    else if (option == "--warmup")
      require(dxmt9::test::parseRenderTapeV4CapacityBytes(value, options.warmup),
          "warmup-must-be-unsigned-decimal");
    else if (option == "--repeat")
      require(dxmt9::test::parseRenderTapeV4CapacityBytes(value, options.repeat),
          "repeat-must-be-unsigned-decimal");
    else if (option == "--reset-policy") options.resetPolicy = value;
    else if (option == "--draw-command-limit")
      require(dxmt9::test::parseRenderTapeV4CapacityBytes(value, options.drawCommandLimit),
          "draw-command-limit-must-be-unsigned-decimal");
    else if (option == "--slot-headroom-bytes")
      require(dxmt9::test::parseRenderTapeV4CapacityBytes(value,
          options.capacityPolicy.slotHeadroomBytes), "slot-headroom-bytes-must-be-unsigned-decimal");
    else if (option == "--aggregate-headroom-bytes")
      require(dxmt9::test::parseRenderTapeV4CapacityBytes(value,
          options.capacityPolicy.aggregateHeadroomBytes), "aggregate-headroom-bytes-must-be-unsigned-decimal");
    else if (option == "--action-profile-sha256") options.actionProfile = parseDigest(value);
    else if (option == "--events-sha256") options.events = parseDigest(value);
    else if (option == "--identity-sha256") options.identity = parseDigest(value);
    else if (option == "--execution-topology-sha256") options.topology = parseDigest(value);
    else if (option == "--source-sha256") options.source = parseDigest(value);
    else if (option == "--output-sha256") options.output = parseDigest(value);
    else if (option == "--expected-plan-digest") options.plan = parseDigest(value);
    else throw std::runtime_error("unknown-option");
  }
  for (const auto* required : {"--bundle", "--events-sha256", "--identity-sha256",
                              "--execution-topology-sha256", "--source-sha256", "--output-sha256"})
    require(seen.contains(required), "required-component-option-missing");
  require(!options.bundle.empty(), "bundle-path-empty");
  require(!seen.contains("--readback-dir") || !options.readbackDirectory.empty(),
      "readback-directory-path-empty");
  require(options.producerContract == "none" ||
      options.producerContract == "pe-present-reservation-v1", "unsupported-producer-contract");
  require(options.publicationMode == "exact-source-diagnostic" ||
      options.publicationMode == "semantic-mapping", "unsupported-publication-mode");
  require(options.publicationMode != "semantic-mapping" || seen.contains("--action-profile-sha256"),
      "semantic-mapping-requires-authenticated-action-profile");
  if (options.inspect) {
    require(!options.observeEncodeDraws, "inspection-cannot-observe-encode-draws");
    require(!seen.contains("--expected-plan-digest") && !seen.contains("--replay-route"),
        "inspection-cannot-request-execution");
    require(!seen.contains("--readback-dir"), "inspection-cannot-write-readbacks");
  } else {
    require(seen.contains("--expected-plan-digest") && seen.contains("--replay-route"),
        "execution-requires-explicit-plan-digest-and-route");
    require(options.route == "production-slot" || options.route == "compatibility",
        "unsupported-replay-route");
  }
  require(dxmt9::test::renderTapeV4CapacityFlagsPaired(seen.contains("--slot-headroom-bytes"),
      seen.contains("--aggregate-headroom-bytes")),
      "capacity-headroom-options-must-be-paired");
  require((!options.capacityPolicy.slotHeadroomBytes && !options.capacityPolicy.aggregateHeadroomBytes) ||
      options.publicationMode == "semantic-mapping", "positive-capacity-requires-semantic-mapping");
  require(!options.drawCommandLimit || options.publicationMode == "semantic-mapping",
      "positive-draw-command-limit-requires-semantic-mapping");
  options.repeated = seen.contains("--warmup") || seen.contains("--repeat") ||
      seen.contains("--reset-policy");
  if (options.repeated) {
    require(seen.contains("--warmup") && seen.contains("--repeat") &&
        seen.contains("--reset-policy"), "repetition-options-must-be-complete");
    require(!options.inspect, "inspection-cannot-request-repetitions");
    require(!options.observeEncodeDraws, "repetitions-cannot-observe-encode-draws");
    require(!options.readbackDirectory.empty(), "repetitions-require-readback-directory");
    require(options.resetPolicy == "fresh-device", "repetitions-require-fresh-device-reset");
    require(options.repeat >= 1u && options.repeat <= 64u && options.warmup <= 64u &&
        options.warmup <= 64u - options.repeat, "repetition-count-out-of-bounds");
  }
  // Inspect admits the requested axis only, not an execution route or an
  // effective runtime policy. Both-zero inspection may use either admission
  // route; a positive draw limit is Compatibility, positive capacity Direct.
  const dxmt9::test::RenderTapeV4ReplayPolicy policy{
      .productionSlot = options.inspect ? options.drawCommandLimit == 0u
                                        : options.route == "production-slot",
      .drawCommandLimit = options.drawCommandLimit, .capacity = options.capacityPolicy};
  require(dxmt9::test::renderTapeV4ReplayPolicyValid(policy), "replay-policy-rejected");
  return options;
}

RenderTapeExecutionV4ProducerContract producerContract(const Options& options) {
  return options.producerContract == "pe-present-reservation-v1"
      ? RenderTapeExecutionV4ProducerContract::PePresentReservationV1
      : RenderTapeExecutionV4ProducerContract::None;
}

RenderTapeExecutionV4PublicationMode publicationMode(const Options& options) {
  return options.publicationMode == "semantic-mapping"
      ? RenderTapeExecutionV4PublicationMode::SemanticMapping
      : RenderTapeExecutionV4PublicationMode::ExactSourceDiagnostic;
}

std::vector<std::byte> readFile(const fs::path& path, std::uint64_t limit) {
  require(fs::symlink_status(path).type() == fs::file_type::regular, "input-not-regular-file");
  std::ifstream stream(path, std::ios::binary | std::ios::ate);
  require(static_cast<bool>(stream), "input-open-failed");
  const auto end = stream.tellg();
  require(end > 0 && static_cast<std::uintmax_t>(end) <= limit, "input-byte-limit");
  std::vector<std::byte> bytes(static_cast<std::size_t>(end));
  stream.seekg(0);
  require(static_cast<bool>(stream.read(reinterpret_cast<char*>(bytes.data()),
      static_cast<std::streamsize>(bytes.size()))), "input-read-failed");
  require(stream.peek() == std::char_traits<char>::eof(), "input-size-changed");
  return bytes;
}

std::vector<std::byte> component(const fs::path& root, const char* name,
    const RenderTapeDigest& expected) {
  auto bytes = readFile(root / name, componentLimit);
  require(RenderTapeCaptureSession::sha256(bytes) == expected, "component-digest-mismatch");
  return bytes;
}

struct Inputs {
  std::vector<std::byte> tape{}, identity{}, topology{}, source{}, output{}, actionProfile{};
  std::vector<std::vector<std::byte>> storage{};
  std::vector<RenderTapeProviderBlob> blobs{};
  RenderTapeBlobCatalogue catalogue{};
  std::uint64_t blobBytes = 0u;
};

Inputs load(const Options& options) {
  require(fs::symlink_status(options.bundle).type() == fs::file_type::directory,
      "bundle-not-directory");
  Inputs input;
  input.tape = component(options.bundle, "events.bin", options.events);
  input.identity = component(options.bundle, "identity.bin", options.identity);
  input.topology = component(options.bundle, "execution_topology.bin", options.topology);
  input.source = component(options.bundle, "source.rgba", options.source);
  input.output = component(options.bundle, "output.rgba", options.output);
  if (std::any_of(options.actionProfile.begin(), options.actionProfile.end(),
          [](auto byte) { return byte != std::byte{}; })) {
    input.actionProfile = readFile(options.bundle / "action_profile.bin", kRenderTapeActionProfileV1Bytes);
    require(RenderTapeCaptureSession::sha256(input.actionProfile) == options.actionProfile,
        "action-profile-digest-mismatch");
  }
  const auto directory = options.bundle / "blobs";
  require(fs::symlink_status(directory).type() == fs::file_type::directory, "blobs-not-directory");
  std::vector<fs::path> paths;
  for (const auto& entry : fs::directory_iterator(directory)) {
    require(paths.size() < kRenderTapeHardMaxBlobEntries, "blob-entry-limit");
    const auto name = entry.path().filename().string();
    require(name.size() == 68u && name.ends_with(".bin"), "blob-name-not-canonical");
    (void)parseDigest(std::string_view(name).substr(0u, 64u));
    require(entry.symlink_status().type() == fs::file_type::regular, "blob-not-regular-file");
    paths.push_back(entry.path());
  }
  std::sort(paths.begin(), paths.end());
  input.storage.reserve(paths.size());
  input.blobs.reserve(paths.size());
  input.catalogue.blobs.reserve(paths.size());
  for (const auto& path : paths) {
    const auto expected = parseDigest(path.stem().string());
    input.storage.push_back(readFile(path, blobLimit - input.blobBytes));
    const auto& bytes = input.storage.back();
    require(RenderTapeCaptureSession::sha256(bytes) == expected, "blob-digest-mismatch");
    input.blobBytes += bytes.size();
    input.blobs.push_back({expected, bytes});
    input.catalogue.blobs.push_back({.digest = expected, .size = bytes.size(), .verified = 1u});
  }
  return input;
}

bool queryQualified(const GeneralPlan& plan, const RenderTapeQueryReplayEvidence& expected,
    const RenderTapeQueryReplayEvidence& actual) {
  if (!expected.present)
    return !actual.present && !actual.gpuResultOracle && !actual.queryDefinitions &&
        !actual.issueRecords && !actual.recordedPolls && actual.rows.empty() && actual.checkpoints.empty();
  if (!actual.present || !actual.actualMetal || !actual.transcriptComplete || !actual.oracleComplete ||
      actual.failure != RenderTapeQueryReplayFailure::None ||
      actual.progressLimit != RenderTapeQueryProgressLimit::None || actual.substitutedResults ||
      actual.queryDefinitions != expected.queryDefinitions || actual.issueRecords != expected.issueRecords ||
      actual.recordedPolls != expected.recordedPolls || actual.replayedIssueRecords != expected.issueRecords ||
      actual.replayedPolls != expected.recordedPolls || actual.rows.size() != expected.recordedPolls ||
      actual.capturedPending != expected.capturedPending || actual.capturedReady != expected.capturedReady ||
      actual.requiredResults != expected.requiredResults || actual.matchedResults != expected.requiredResults ||
      actual.readinessRequired != expected.readinessRequired ||
      actual.readinessSatisfied != expected.readinessRequired ||
      actual.gpuResultOracle != (expected.capturedReady != 0u) ||
      actual.maxOraclePolls > 262144u || actual.supplementalPolls > actual.maxOraclePolls ||
      actual.maxOracleWallNanos > 5000000000ull || actual.oracleWallNanos > actual.maxOracleWallNanos)
    return false;
  std::size_t next = 0u;
  for (const auto& event : plan.events) {
    if (event.type != RenderTapeEventType::OrderedControl) continue;
    const auto& fixed = plan.controls[event.index];
    if (fixed.kind != static_cast<std::uint32_t>(RenderTapeControlKind::QueryGetData)) continue;
    const auto& bytes = plan.controlBytes[event.index];
    if (next >= actual.rows.size() || bytes.size() != sizeof(RenderTapeQueryGetDataControlV2)) return false;
    RenderTapeQueryGetDataControlV2 poll{};
    std::memcpy(&poll, bytes.data(), sizeof(poll));
    const auto& row = actual.rows[next++];
    if (row.eventIndex != event.eventIndex || row.eventOrdinal != plan.tape.event(event.eventIndex).header.ordinal ||
        !renderTapeIdentityV3SameIdentity(row.identity, fixed.identity) || row.issueCycle != poll.issueCycle ||
        row.flags != poll.flags || row.hasOutput != (poll.hasOutput != 0u) || row.dataSize != poll.dataSize ||
        row.capturedResult != fixed.resultCode || row.definedBytes != poll.definedBytes ||
        row.capturedBytes != poll.result || row.resultRequired != (fixed.resultCode == 0) ||
        row.resultSubstituted || !row.localQueryIdentity ||
        (fixed.resultCode < 0 && row.replayResult != fixed.resultCode) ||
        (fixed.resultCode >= 0 && row.replayResult != 0 && row.replayResult != 1)) return false;
    if (row.resultRequired && (!row.readinessSatisfied ||
        (row.definedBytes && (!row.resultMatched || !std::equal(row.capturedBytes.begin(),
            row.capturedBytes.begin() + row.definedBytes, row.replayBytes.begin()))))) return false;
  }
  if (next != actual.rows.size() || actual.checkpointDefinitions != expected.checkpointDefinitions ||
      actual.checkpoints.size() != expected.checkpointDefinitions ||
      actual.checkpointSetupEnds != expected.checkpointDefinitions ||
      actual.checkpointCompleted != expected.checkpointDefinitions ||
      actual.checkpointActivated != expected.checkpointDefinitions) return false;
  for (const auto& row : actual.checkpoints)
    if (!row.setupIssued || !row.completed || !row.activated || !row.localQueryIdentity) return false;
  return true;
}

struct ReadbackArtifact {
  bool attempted = false, written = false, digestComputed = false;
  std::uint64_t bytes = 0u, writtenBytes = 0u;
  RenderTapeDigest digest{};
  const char* failure = "none";
};

struct ReadbackArtifacts {
  bool requested = false, attempted = false;
  ReadbackArtifact source{}, output{};
  const char* failure = "none";
};

namespace witness = dxmt9::tests::encode_draw_witness;

struct EncodeDrawWitnessOwner {
  witness::Collector collector{};
  dxmt9::test::RenderTapeEncodeOriginalAdapter original;
  dxmt9::encoders::EncodeDrawRecorder recorder{};

  EncodeDrawWitnessOwner(std::vector<std::vector<std::byte>> storage,
      std::vector<RenderTapeProviderBlob> blobs) noexcept
      : original(std::move(storage), std::move(blobs)) {
    recorder.userdata = this;
    // Every suppression/fake/split/upload override stays at its false/null
    // default. These callbacks observe the renderer's natural actual calls.
    recorder.beginDraw = [](void* context, std::uint64_t sequence,
        std::uint32_t command, std::uint64_t subdraw) noexcept {
      auto& self = *static_cast<EncodeDrawWitnessOwner*>(context);
      self.collector.suppression(self.recorder.suppressMetalCalls,
          self.recorder.suppressCommandEncoderSideEffects, self.recorder.suppressBaseStateLookup);
      self.collector.beginDraw({sequence, command, subdraw});
      self.original.beginDraw({sequence, command, subdraw});
    };
    recorder.observeSourceBindingContext = [](void* context,
        const dxmt9::core::CpuReadyTape::SourceRef& source, std::uint64_t sequence) noexcept {
      static_cast<EncodeDrawWitnessOwner*>(context)->original.observeSource(source, sequence);
    };
    recorder.observeDrawBindingContext = [](void* context,
        const dxmt9::core::FlatDrawStateRecord& hot,
        const dxmt9::core::DrawShaderLayoutContext& layout,
        const dxmt9::core::DrawParam& parameters,
        const dxmt9::core::DrawBufferBindingSnapshot* vertex,
        const dxmt9::core::DrawBufferBindingSnapshot* index) noexcept {
      static_cast<EncodeDrawWitnessOwner*>(context)->original.observeDrawContext(
          hot, layout, parameters, vertex, index);
    };
    recorder.beginSourceFragmentPreamble = [](void* context, std::uint64_t sequence) {
      static_cast<EncodeDrawWitnessOwner*>(context)->collector.beginSourceFragment(sequence);
    };
    recorder.endSourceFragmentEpilogue = [](void* context, std::uint64_t sequence, std::uint64_t) {
      static_cast<EncodeDrawWitnessOwner*>(context)->collector.endSourceFragment(sequence);
    };
    recorder.beginRenderPass = [](void* context, std::size_t) {
      static_cast<EncodeDrawWitnessOwner*>(context)->collector.renderPassBoundary();
    };
    recorder.endRenderPass = [](void* context) {
      static_cast<EncodeDrawWitnessOwner*>(context)->collector.renderPassBoundary();
    };
    recorder.observeBufferSlice = [](void* context,
        dxmt9::encoders::EncodeDrawBufferObservationKind kind, std::uint8_t reason,
        std::uint64_t source, std::span<const std::uint8_t> input, WMT::Buffer buffer,
        std::uint64_t offset, std::uint64_t size) noexcept {
      auto& collector = static_cast<EncodeDrawWitnessOwner*>(context)->collector;
      if (collector.failed()) return;
      using Kind = dxmt9::encoders::EncodeDrawBufferObservationKind;
      using Status = dxmt9::test::CpuTapeEncodeDestinationStatus;
      // Enum values are a private recorder alphabet, not a PE/wire ABI.
      static_assert(static_cast<unsigned>(Kind::TransientVertexUpload) ==
          static_cast<unsigned>(witness::BufferKind::TransientVertexUpload));
      static_assert(static_cast<unsigned>(Kind::TransientIndexUpload) ==
          static_cast<unsigned>(witness::BufferKind::TransientIndexUpload));
      static_assert(static_cast<unsigned>(Kind::StagedVertexUpload) ==
          static_cast<unsigned>(witness::BufferKind::StagedVertexUpload));
      static_assert(static_cast<unsigned>(Kind::StagedIndexUpload) ==
          static_cast<unsigned>(witness::BufferKind::StagedIndexUpload));
      static_assert(static_cast<unsigned>(Kind::StagedVertexReuse) ==
          static_cast<unsigned>(witness::BufferKind::StagedVertexReuse));
      static_assert(static_cast<unsigned>(Kind::StagedIndexReuse) ==
          static_cast<unsigned>(witness::BufferKind::StagedIndexReuse));
      witness::DestinationCheck destination;
      if (kind != Kind::StagedVertexReuse && kind != Kind::StagedIndexReuse) {
        const auto compared = dxmt9::test::compareCpuTapeEncodeDestination(
            buffer, offset, size, input, collector.destinationBudget());
        destination.status = static_cast<std::uint8_t>(compared.status);
        destination.comparedBytes = compared.comparedBytes;
        destination.state = compared.equal() ? witness::DestinationCheck::State::Equal
            : compared.status == Status::UnsupportedStorage || compared.status == Status::ContentsUnavailable
                ? witness::DestinationCheck::State::Unavailable : witness::DestinationCheck::State::Rejected;
      }
      collector.observeSlice(static_cast<witness::BufferKind>(kind), reason, source,
          input, {buffer.handle, offset, size}, destination);
    };
    recorder.setVertexBuffer = [](void* context, WMT::Buffer buffer,
        std::uint64_t offset, std::uint8_t slot) {
      auto& self = *static_cast<EncodeDrawWitnessOwner*>(context);
      self.collector.bindVertex(buffer.handle, offset, slot);
      self.original.observeVertexBuffer(slot);
    };
    recorder.setVertexBytes = [](void* context, const void* bytes, std::uint64_t size, std::uint8_t slot) {
      auto& self = *static_cast<EncodeDrawWitnessOwner*>(context);
      self.collector.bindVertexBytes(slot);
      self.original.observeVertexBytes(bytes, size, slot);
    };
    recorder.drawPrimitives = [](void* context, WMTPrimitiveType primitive,
        std::uint64_t start, std::uint64_t count, std::uint32_t instances, std::uint32_t baseInstance) {
      auto& self = *static_cast<EncodeDrawWitnessOwner*>(context);
      self.collector.issue(false,
          static_cast<std::uint32_t>(primitive), count, start, instances, 0, baseInstance);
      if (!self.collector.failed()) self.original.observeIssue(self.collector.issues().back());
    };
    recorder.drawIndexedPrimitives = [](void* context, WMTPrimitiveType primitive, WMTIndexType type,
        std::uint64_t count, WMT::Buffer buffer, std::uint64_t offset, std::uint32_t instances,
        std::int32_t baseVertex, std::uint32_t baseInstance) {
      auto& self = *static_cast<EncodeDrawWitnessOwner*>(context);
      self.collector.issue(true,
          static_cast<std::uint32_t>(primitive), count, 0u, instances, baseVertex, baseInstance,
          {buffer.handle, offset, 0u}, type == WMTIndexTypeUInt16 ? 2u : type == WMTIndexTypeUInt32 ? 4u : 0u);
      if (!self.collector.failed()) self.original.observeIssue(self.collector.issues().back());
    };
  }
};

struct EncodeDrawIdentityJoin {
  std::uint32_t sourceIndex = noIndex;
  std::uint64_t sourceOrdinal = 0u, rawOrdinal = 0u;
  std::uint64_t writerGeneration = 0u, storageGeneration = 0u;
  bool sourceComplete = false;
  std::uint32_t originalMatches = 0u;
  std::uint64_t originalEvent = 0u;
  std::uint32_t originalRecord = noIndex, originalType = 0u;
};

struct Evidence {
  Options options{};
  const char* phase = "arguments";
  std::string failure{};
  bool inputsOwned = false, pairValid = false, preflightValid = false, admitted = false;
  bool oracleInputsValid = false, digestMatched = false, deviceCreated = false, actualMetal = false;
  bool teardownComplete = false, countersObserved = false, countersMonotonic = false;
  bool providerCallReturned = false;
  bool query = false, sourceExact = false, outputExact = false, pixels = false, qualified = false;
  bool actionProfileAuthenticated = false, actionProfileStableExact = false;
  bool semanticMappingQualified = false, commandGroupingChanged = false;
  struct LoadedImage {
    std::string path{};
    std::uint64_t bytes = 0u;
    RenderTapeDigest digest{};
    bool operator==(const LoadedImage&) const = default;
  };
  std::vector<LoadedImage> loadedImages{};
  std::map<std::string, std::string> effectiveStageEnvironment{};
  RenderTapeActionWitnessV1 actionWitness{};
  std::uint32_t executionCount = 0u, failedRecord = noIndex;
  std::uint32_t iterationOrdinal = noIndex, iterationKindIndex = 0u;
  const char* iterationKind = "none";
  std::uint64_t blobBytes = 0u, expectedPixels = 0u;
  std::uint64_t providerCallWallNanos = 0u;
  RenderTapeDigest planDigest{};
  RenderTapeValidationResult wire{};
  FrameTapeReplayResult preflight{}, result{};
  RenderTapeExecutionV4Evidence admission{};
  CaptureSourceReplayPlanV4Status planStatus = CaptureSourceReplayPlanV4Status::InvalidInput;
  Counters before{}, after{};
  std::size_t planEvents = 0u, planRecords = 0u, planSources = 0u, planCommands = 0u,
      planCoordinators = 0u;
  std::array<std::uint32_t, 5u> queryTypes{}; // D3D EVENT through TIMESTAMPFREQ.
  // Bounded reporting only; these copies never participate in qualification.
  std::array<RenderTapeIdentityV3Source, 8u> expectedSources{};
  std::array<RenderTapeIdentityV3Initializer, 8u> expectedInitializers{};
  std::size_t reportedSources = 0u, reportedInitializers = 0u, planInitializers = 0u;
  ReadbackArtifacts readbacks{};
  // This owner is outside execute's local device/factory scope, including
  // exceptional unwind. Queue teardown joins encode callbacks before reads.
  std::unique_ptr<EncodeDrawWitnessOwner> encodeDrawWitness{};
  bool encodeDrawWitnessInstalled = false;
  std::vector<EncodeDrawIdentityJoin> encodeDrawJoins{};
  const char* encodeDrawJoinFailure = "none";
};

class ArtifactFileDescriptor {
public:
  ArtifactFileDescriptor() = default;
  ~ArtifactFileDescriptor() { reset(); }
  ArtifactFileDescriptor(const ArtifactFileDescriptor&) = delete;
  ArtifactFileDescriptor& operator=(const ArtifactFileDescriptor&) = delete;
  int get() const noexcept { return value_; }
  int release() noexcept { return std::exchange(value_, -1); }
  void reset(int value = -1) noexcept {
    if (value_ >= 0) (void)::close(value_);
    value_ = value;
  }
private:
  int value_ = -1;
};

std::vector<fs::path> loadedImagePaths() {
#if defined(__APPLE__)
  const auto count = _dyld_image_count();
  require(count > 0u, "loaded-image-executable-unavailable");
  std::set<fs::path> paths;
  bool nativeLibrary = false, unixModule = false;
  for (std::uint32_t index = 0u; index < count; ++index) {
    const auto* name = _dyld_get_image_name(index);
    require(name && *name, "loaded-image-name-unavailable");
    const fs::path observed(name);
    const auto basename = observed.filename().string();
    if (index != 0u && basename.find("dxmt9") == std::string::npos &&
        basename.find("winemetal") == std::string::npos) continue;
    const auto path = fs::canonical(observed);
    require(path.is_absolute() && !path.empty(), "loaded-image-path-not-canonical");
    require(fs::symlink_status(path).type() == fs::file_type::regular,
        "loaded-image-not-regular-file");
    require(path.native().size() <= 4096u, "loaded-image-path-limit");
    paths.insert(path);
    require(paths.size() <= loadedImageCountLimit, "loaded-image-count-limit");
    // These are required actual linked runtime images, not rows synthesized
    // from a build directory or argv[0]. Index zero is the dyld executable.
    nativeLibrary |= index != 0u && basename == "libdxmt9_native.dylib";
    unixModule |= index != 0u && basename == "winemetal_dxmt9.so";
  }
  require(nativeLibrary && unixModule, "loaded-image-runtime-module-unavailable");
  return {paths.begin(), paths.end()};
#else
  throw std::runtime_error("loaded-image-enumeration-unavailable");
#endif
}

bool sameLoadedImageFile(const struct stat& left, const struct stat& right) {
  return left.st_dev == right.st_dev && left.st_ino == right.st_ino &&
      left.st_mode == right.st_mode && left.st_size == right.st_size &&
#if defined(__APPLE__)
      left.st_mtimespec.tv_sec == right.st_mtimespec.tv_sec &&
      left.st_mtimespec.tv_nsec == right.st_mtimespec.tv_nsec &&
      left.st_ctimespec.tv_sec == right.st_ctimespec.tv_sec &&
      left.st_ctimespec.tv_nsec == right.st_ctimespec.tv_nsec;
#else
      left.st_mtim.tv_sec == right.st_mtim.tv_sec &&
      left.st_mtim.tv_nsec == right.st_mtim.tv_nsec &&
      left.st_ctim.tv_sec == right.st_ctim.tv_sec &&
      left.st_ctim.tv_nsec == right.st_ctim.tv_nsec;
#endif
}

std::vector<Evidence::LoadedImage> collectLoadedImages() {
  const auto paths = loadedImagePaths();
  std::vector<Evidence::LoadedImage> images;
  images.reserve(paths.size());
  std::uint64_t total = 0u;
  for (const auto& path : paths) {
    ArtifactFileDescriptor file;
    file.reset(::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW));
    require(file.get() >= 0, "loaded-image-open-failed");
    struct stat before{};
    require(::fstat(file.get(), &before) == 0 && S_ISREG(before.st_mode),
        "loaded-image-not-regular-file");
    require(before.st_size > 0 &&
        static_cast<std::uint64_t>(before.st_size) <= loadedImageByteLimit - total,
        "loaded-image-byte-limit");
    const auto size = static_cast<std::uint64_t>(before.st_size);
    std::vector<std::byte> bytes(static_cast<std::size_t>(size));
    std::size_t offset = 0u;
    while (offset < bytes.size()) {
      const auto count = ::read(file.get(), bytes.data() + offset, bytes.size() - offset);
      if (count < 0 && errno == EINTR) continue;
      require(count > 0, "loaded-image-read-failed");
      offset += static_cast<std::size_t>(count);
    }
    std::byte extra{};
    ssize_t end = -1;
    do { end = ::read(file.get(), &extra, 1u); } while (end < 0 && errno == EINTR);
    require(end == 0, "loaded-image-size-changed");
    const auto digest = RenderTapeCaptureSession::sha256(bytes);
    struct stat after{}, named{};
    require(::fstat(file.get(), &after) == 0 && ::lstat(path.c_str(), &named) == 0 &&
        sameLoadedImageFile(before, after) && sameLoadedImageFile(after, named) &&
        fs::canonical(path) == path, "loaded-image-changed-during-authentication");
    images.push_back({path.string(), size, digest});
    total += size;
  }
  // dyld iteration itself is not an atomic snapshot. Reject any observed
  // relevant module change while file fingerprints were being collected.
  require(paths == loadedImagePaths(), "loaded-image-set-changed-during-authentication");
  return images;
}

const char* openReadbackDirectory(const fs::path& path,
    ArtifactFileDescriptor& directory) {
  const auto& text = path.native();
  if (text.empty() || text.size() > 4096u || text.find('\0') != std::string::npos ||
      path.filename().empty()) return "readback-directory-path-invalid";
  constexpr int flags = O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW;
  directory.reset(::open(path.is_absolute() ? "/" : ".", flags));
  if (directory.get() < 0) return "readback-directory-open-failed";
  // Resolve every component through pinned descriptors. A symlink or parent
  // traversal must not redirect an artifact into an input or another owner.
  for (const auto& component : path) {
    if (component == path.root_directory()) continue;
    const auto name = component.native();
    if (name.empty() || name == "." || name == "..")
      return "readback-directory-path-invalid";
    const auto next = ::openat(directory.get(), name.c_str(), flags);
    if (next < 0) return "readback-directory-not-plain-directory";
    directory.reset(next);
  }
  struct stat status{};
  if (::fstat(directory.get(), &status) != 0 || !S_ISDIR(status.st_mode))
    return "readback-directory-not-plain-directory";
  if (status.st_uid != ::geteuid()) return "readback-directory-not-owned";
  if (status.st_mode & (S_IRWXG | S_IRWXO)) return "readback-directory-not-private";
  return "none";
}

void describeReadback(std::span<const std::byte> bytes, ReadbackArtifact& artifact) {
  artifact.bytes = bytes.size();
  if (!bytes.empty() && bytes.size() <= componentLimit) {
    artifact.digest = RenderTapeCaptureSession::sha256(bytes);
    artifact.digestComputed = true;
  }
}

void writeReadback(int directory, const char* name,
    std::span<const std::byte> bytes, ReadbackArtifact& artifact) noexcept {
  artifact.attempted = true;
  if (bytes.empty()) { artifact.failure = "readback-unavailable"; return; }
  if (bytes.size() > componentLimit) { artifact.failure = "readback-byte-limit"; return; }
  ArtifactFileDescriptor file;
  file.reset(::openat(directory, name, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC,
      S_IRUSR | S_IWUSR));
  if (file.get() < 0) {
    artifact.failure = errno == EEXIST ? "readback-file-exists" : "readback-file-open-failed";
    return;
  }
  struct stat status{};
  if (::fstat(file.get(), &status) != 0 || !S_ISREG(status.st_mode) ||
      status.st_uid != ::geteuid() || status.st_nlink != 1) {
    artifact.failure = "readback-file-not-owned-regular";
    return;
  }
  while (artifact.writtenBytes < bytes.size()) {
    const auto remaining = bytes.size() - static_cast<std::size_t>(artifact.writtenBytes);
    const auto amount = std::min<std::size_t>(remaining, 1024u * 1024u);
    const auto written = ::write(file.get(), bytes.data() + artifact.writtenBytes, amount);
    if (written < 0 && errno == EINTR) continue;
    if (written <= 0) { artifact.failure = "readback-file-write-failed"; return; }
    artifact.writtenBytes += static_cast<std::uint64_t>(written);
  }
  if (::fsync(file.get()) != 0) { artifact.failure = "readback-file-sync-failed"; return; }
  if (::close(file.release()) != 0) { artifact.failure = "readback-file-close-failed"; return; }
  artifact.written = true;
}

void preserveReadbacks(Evidence& evidence) {
  auto& artifacts = evidence.readbacks;
  artifacts.requested = !evidence.options.readbackDirectory.empty();
  if (!artifacts.requested) return;
  artifacts.attempted = true;
  try {
    describeReadback(evidence.result.sourcePixels, artifacts.source);
    describeReadback(evidence.result.outputPixels, artifacts.output);
    ArtifactFileDescriptor directory;
    artifacts.failure = openReadbackDirectory(evidence.options.readbackDirectory, directory);
    if (std::strcmp(artifacts.failure, "none") != 0) {
      artifacts.source.failure = artifacts.output.failure = artifacts.failure;
    } else {
      writeReadback(directory.get(), "replay-source.rgba", evidence.result.sourcePixels, artifacts.source);
      writeReadback(directory.get(), "replay-output.rgba", evidence.result.outputPixels, artifacts.output);
      artifacts.failure = std::strcmp(artifacts.source.failure, "none") != 0
          ? artifacts.source.failure : artifacts.output.failure;
    }
  } catch (...) {
    artifacts.failure = "readback-artifact-exception";
  }
  if (std::strcmp(artifacts.failure, "none") != 0) {
    evidence.qualified = false;
    // Artifact failures cannot conceal the original rendering failure.
    if (evidence.failure.empty()) {
      evidence.failure = artifacts.failure;
      evidence.phase = "readback-artifacts";
    }
  }
}

void recordFailureLocator(Evidence& evidence, std::span<const std::byte> tape) {
  const auto index = evidence.preflight.failedEventIndex != noIndex
      ? evidence.preflight.failedEventIndex : evidence.wire.failedEventIndex;
  ImportedRenderTapeView imported;
  if (index == noIndex || !importPrevalidatedRenderTape(tape, imported) || index >= imported.events.size()) return;
  const auto event = imported.event(index);
  if (event.header.type != static_cast<std::uint32_t>(RenderTapeEventType::CommandChunk) ||
      event.payload.size() < sizeof(RenderTapeCommandChunkHeader)) return;
  RenderTapeCommandChunkHeader fixed{};
  std::memcpy(&fixed, event.payload.data(), sizeof(fixed));
  evidence.failedRecord = validateCommandChunk(event.payload.subspan(sizeof(fixed)),
      {fixed.wireVersion, fixed.recordCount, fixed.handleCount}).failedRecordIndex;
}

bool monotonic(const Counters& before, const Counters& after) {
  for (const auto& [a, b] : {std::pair{before.prepareSlotWallNs, after.prepareSlotWallNs},
          std::pair{before.resourceMarkWallNs, after.resourceMarkWallNs},
          std::pair{before.preparedSerialBuildWallNs, after.preparedSerialBuildWallNs},
          std::pair{before.encodeChunkWallNs, after.encodeChunkWallNs},
          std::pair{before.directLeases, after.directLeases},
          std::pair{before.directLeaseDraws, after.directLeaseDraws},
          std::pair{before.compatibilityDraws, after.compatibilityDraws},
          std::pair{before.compatibilityBatches, after.compatibilityBatches},
          std::pair{before.compatibilityUnbatchedDraws, after.compatibilityUnbatchedDraws},
          std::pair{before.encodeChunks, after.encodeChunks}, std::pair{before.gpuErrors, after.gpuErrors}})
    if (b < a) return false;
  return true;
}

void joinEncodeDrawIdentities(Evidence& evidence) noexcept {
  if (!evidence.encodeDrawWitness) return;
  try {
    const auto& v4 = evidence.result.exactDiagnosticV4.execution;
    const auto& actual = v4.actualIdentity;
    const auto issues = evidence.encodeDrawWitness->collector.issues();
    if (actual.sources.size() > 65536u || issues.size() > 65536u) {
      evidence.encodeDrawJoinFailure = "encode-identity-join-capacity"; return;
    }
    std::map<std::uint64_t, std::uint32_t> sequences;
    if (v4.actualLifecycleComplete) {
      for (std::size_t index = 0u; index < actual.sources.size(); ++index) {
        const auto sequence = actual.sources[index].lifecycle.seqId;
        const auto [position, inserted] = sequences.emplace(sequence, static_cast<std::uint32_t>(index));
        if (!inserted) position->second = noIndex;
      }
    }
    using Key = std::tuple<std::uint32_t, std::uint32_t, std::uint64_t>;
    struct Origin {
      std::uint32_t matches = 0u;
      std::uint64_t event = 0u;
      std::uint32_t record = noIndex, type = 0u;
    };
    std::map<Key, Origin> origins;
    // Only the owned authenticated semantic record map exposes the original
    // record directly. Exact source mappings alone do not authorize inferred
    // draw ordinal -> original-record correspondence.
    const bool mapping = v4.semanticMappingComplete && v4.semanticMapping &&
        v4.semanticMapping->valid() && v4.semanticMapping->planDigest() == evidence.options.plan;
    if (mapping) {
      if (v4.semanticMapping->records().size() > 65536u) {
        evidence.encodeDrawJoinFailure = "encode-origin-join-capacity"; return;
      }
      for (const auto& record : v4.semanticMapping->records()) {
        if (record.candidateSourceIndex == noIndex || record.candidatePhysicalCommandIndex == noIndex ||
            record.candidateSubdraw == noIndex) continue;
        auto& origin = origins[{record.candidateSourceIndex, record.candidatePhysicalCommandIndex,
                                record.candidateSubdraw}];
        ++origin.matches;
        origin.event = record.eventOrdinal;
        origin.record = record.recordIndex;
        origin.type = record.recordType;
      }
    }
    evidence.encodeDrawJoins.reserve(issues.size());
    for (const auto& issue : issues) {
      EncodeDrawIdentityJoin row;
      const auto found = sequences.find(issue.draw.sequence);
      if (found != sequences.end() && found->second != noIndex) {
        row.sourceIndex = found->second;
        const auto& source = actual.sources[row.sourceIndex];
        row.sourceOrdinal = source.lifecycle.sourceOrdinal;
        row.rawOrdinal = source.rawOrdinal;
        row.writerGeneration = source.lifecycle.writerGeneration;
        row.storageGeneration = source.lifecycle.storageGeneration;
        row.sourceComplete = source.lifecycle.submissionEdge && source.lifecycle.completionEdge &&
            source.lifecycle.completedSequence >= issue.draw.sequence;
        const auto original = origins.find({row.sourceIndex, issue.draw.command, issue.draw.subdraw});
        if (original != origins.end()) {
          row.originalMatches = original->second.matches;
          if (row.originalMatches == 1u) {
            row.originalEvent = original->second.event;
            row.originalRecord = original->second.record;
            row.originalType = original->second.type;
          }
        }
      }
      evidence.encodeDrawJoins.push_back(row);
    }
  } catch (...) {
    evidence.encodeDrawJoins.clear();
    evidence.encodeDrawJoinFailure = "encode-identity-join-allocation";
  }
}

struct DiagnosticPreparedInput {
  Inputs input{};
  GeneralPlan general{};
  CaptureSourceReplayPlanV4 plan{};
  PreparedRenderTape prepared{};
};

void prepareDiagnostic(Evidence& evidence, DiagnosticPreparedInput& session) {
  evidence.phase = "input-authentication";
  auto& input = session.input;
  input = load(evidence.options);
  evidence.inputsOwned = true;
  evidence.blobBytes = input.blobBytes;
  evidence.phase = "wire-preflight";
  evidence.wire = validateRenderTape(input.tape, input.catalogue);
  if (!evidence.wire.valid()) {
    recordFailureLocator(evidence, input.tape);
    throw std::runtime_error("wire-preflight-rejected");
  }
  evidence.phase = "general-preflight";
  auto& general = session.general;
  evidence.preflight = render_tape_runtime::buildGeneralReplayPlan(input.tape, input.blobs, general);
  if (!evidence.preflight.complete()) {
    recordFailureLocator(evidence, input.tape);
    throw std::runtime_error("general-preflight-rejected");
  }
  evidence.preflightValid = true;
  for (const auto& definition : general.definitions) {
    if (definition.fixed.identity.kind != D9C_CHUNK_HANDLE_KIND_QUERY) continue;
    RenderTapeQueryDescriptorDecoded query{};
    if (renderTapeLoadQueryDescriptor(definition.descriptor, query) && query.type >= 8u && query.type <= 12u)
      ++evidence.queryTypes[query.type - 8u];
  }
  require(evidence.preflight.profile == kRenderTapeProfileFrame && evidence.preflight.coverage.commandRecords &&
      evidence.preflight.coverage.drawPrimitiveRecords + evidence.preflight.coverage.drawIndexedPrimitiveRecords +
          evidence.preflight.coverage.drawPrimitiveUpRecords > 0u, "nonempty-general-frame-required");
  require(evidence.preflight.conservation.inputBlobs == evidence.preflight.conservation.referencedBlobs,
      "blob-conservation-preflight-rejected");
  evidence.phase = "paired-plan-authentication";
  auto& plan = session.plan;
  evidence.planStatus = buildCaptureSourceReplayPlanV4(input.tape, input.catalogue,
      input.identity, input.topology, plan);
  require(evidence.planStatus == CaptureSourceReplayPlanV4Status::Valid, "paired-plan-rejected");
  require(plan.identity().header.authority == static_cast<std::uint32_t>(RenderTapeIdentityAuthority::Capture),
      "original-capture-authority-required");
  evidence.pairValid = true;
  evidence.planDigest = plan.digest();
  evidence.planEvents = plan.events().size();
  evidence.planRecords = plan.records().size();
  evidence.planSources = plan.identity().sources.size();
  evidence.planCommands = plan.identity().commands.size();
  evidence.planCoordinators = plan.identity().coordinators.size();
  evidence.planInitializers = plan.identity().initializers.size();
  evidence.reportedSources = std::min(evidence.expectedSources.size(), evidence.planSources);
  evidence.reportedInitializers = std::min(evidence.expectedInitializers.size(), evidence.planInitializers);
  for (std::size_t i = 0u; i < evidence.reportedSources; ++i)
    evidence.expectedSources[i] = plan.identity().sources[i].lifecycle;
  std::copy_n(plan.identity().initializers.begin(), evidence.reportedInitializers,
      evidence.expectedInitializers.begin());
  evidence.phase = "action-profile-authentication";
  if (!input.actionProfile.empty()) {
    RenderTapeActionProfileV1ComponentHeader actionProfile;
    const auto& header = plan.identity().header;
    require(importRenderTapeActionProfileV1Component(input.actionProfile, actionProfile) &&
        renderTapeActionProfileV1ComponentMatchesBinding(actionProfile,
            header.captureToken, header.captureEpoch, header.queue,
            input.tape.size(), input.identity.size(), input.topology.size(),
            RenderTapeCaptureSession::sha256(input.tape), RenderTapeCaptureSession::sha256(input.identity),
            RenderTapeCaptureSession::sha256(input.topology)), "action-profile-binding-rejected");
    evidence.actionProfileAuthenticated = true;
    evidence.actionWitness = actionProfile.witness;
    evidence.actionProfileStableExact = renderTapeActionWitnessV1StableExact(actionProfile.witness);
  }
  require(evidence.options.publicationMode != "semantic-mapping" ||
      (evidence.actionProfileAuthenticated && evidence.actionProfileStableExact),
      "semantic-mapping-requires-stable-exact-action-witness");
  evidence.phase = "v4-diagnostic-admission";
  RenderTapeExecutionV4 admission;
  if (!admission.admit(plan, general, input.tape, evidence.admission,
          publicationMode(evidence.options),
          producerContract(evidence.options)))
    throw std::runtime_error(admission.failure());
  evidence.admitted = true;
  evidence.phase = "original-oracle-admission";
  const auto& dimensions = evidence.preflight.requirements;
  require(dimensions.outputWidth && dimensions.outputHeight &&
      static_cast<std::uint64_t>(dimensions.outputWidth) <= componentLimit / 4u / dimensions.outputHeight,
      "oracle-dimensions-out-of-bounds");
  evidence.expectedPixels = static_cast<std::uint64_t>(dimensions.outputWidth) * dimensions.outputHeight * 4u;
  require(input.source.size() == evidence.expectedPixels && input.output.size() == evidence.expectedPixels,
      "original-oracle-size-mismatch");
  require(general.expectedDigestValidity == RenderTapeDigestValidity::Sha256 &&
      general.expectedDigest == evidence.options.output, "original-output-digest-not-bound-to-tape");
  evidence.oracleInputsValid = true;
  if (evidence.options.inspect) { evidence.phase = "inspection-complete"; return; }
  evidence.phase = "expected-plan-digest";
  require(evidence.planDigest == evidence.options.plan, "expected-plan-digest-mismatch");
  evidence.digestMatched = true;
  evidence.phase = "prepared-owner";
  auto& prepared = session.prepared;
  prepared = prepareRenderTapeIdentityV4(input.tape, input.blobs, {
      .sourceBoundaryIdentity = input.identity, .sourceExecutionTopology = input.topology,
      .expectedSourcePlanDigest = evidence.options.plan,
      .publicationMode = publicationMode(evidence.options),
      .producerContract = producerContract(evidence.options)});
  require(prepared.valid() && prepared.prepareCount() == 1u && prepared.evidence().immutableInputOwned &&
      prepared.evidence().immutableV4PreparationOwned && prepared.evidence().v4PreparationCount == 1u,
      "prepared-owner-rejected");
  if (evidence.options.observeEncodeDraws) {
    // Transfer the authenticated loader owner; the adapter never borrows the
    // prepared provider input or derives canonical expected bytes from Pool.
    evidence.encodeDrawWitness = std::make_unique<EncodeDrawWitnessOwner>(
        std::move(input.storage), std::move(input.blobs));
    require(!evidence.encodeDrawWitness->collector.failed(), "encode-draw-witness-allocation");
    require(!evidence.encodeDrawWitness->original.failed(),
        evidence.encodeDrawWitness->original.contextFailure());
  }
  // The owned prepared input retains all blob bytes. Without observation the
  // loader copy is released; with it that copy belongs to the private adapter.
  // A repeated session also retains the loader owner for every borrowed
  // General-plan span; only the provider's immutable owner enters replay.
  if (!evidence.options.repeated) {
    input.blobs.clear();
    input.storage.clear();
  }
}

void executeIteration(Evidence& evidence, DiagnosticPreparedInput& session) {
  auto& input = session.input;
  const auto& general = session.general;
  const auto& plan = session.plan;
  const auto& prepared = session.prepared;
  const auto& dimensions = evidence.preflight.requirements;
  evidence.phase = "hermetic-diagnostic-environment";
  require(::setenv("DXMT_DEBUG_DISABLE_SHADER_ARCHIVE", "1", 1) == 0 &&
      ::setenv("DXMT9_PREWARM", "disabled", 1) == 0 && ::setenv("DXMT_PERF_COUNTERS", "1", 1) == 0,
      "diagnostic-environment-failed");
  for (const auto& [name, expected] : std::array{
      std::pair{"DXMT_DEBUG_DISABLE_SHADER_ARCHIVE", "1"},
      std::pair{"DXMT9_PREWARM", "disabled"}, std::pair{"DXMT_PERF_COUNTERS", "1"}}) {
    const auto* actual = ::getenv(name);
    require(actual && std::string_view(actual) == expected, "diagnostic-environment-mismatch");
    evidence.effectiveStageEnvironment.emplace(name, actual);
  }
  const std::array replayPolicyEnvironment{
      std::pair{"DXMT9_DRAW_CHUNK_COMMAND_LIMIT",
          std::to_string(evidence.options.drawCommandLimit)},
      std::pair{"DXMT9_CHUNK_DRAW_PAYLOAD_ARENA_LIMIT_BYTES", std::string("0")},
      std::pair{"DXMT9_DIRECT_SLOT_HEADROOM_BYTES",
          std::to_string(evidence.options.capacityPolicy.slotHeadroomBytes)},
      std::pair{"DXMT9_DIRECT_SLOT_AGGREGATE_HEADROOM_BYTES",
          std::to_string(evidence.options.capacityPolicy.aggregateHeadroomBytes)}};
  for (const auto& [name, expected] : replayPolicyEnvironment) {
    require(::setenv(name, expected.c_str(), 1) == 0, "diagnostic-replay-policy-environment-failed");
    const auto* actual = ::getenv(name);
    require(actual && std::string_view(actual) == expected, "diagnostic-replay-policy-environment-mismatch");
    evidence.effectiveStageEnvironment.emplace(name, actual);
  }
  // Only the admitted existing grouping or retained-capacity selector changes;
  // the payload byte cap is pinned off to exclude inherited grouping drift.
  // No encoder partition or query cut is introduced. Requested ProductionSlot
  // may naturally fall back/mix routes.
  const D9CPresentParams params{.backBufferWidth = dimensions.outputWidth,
      .backBufferHeight = dimensions.outputHeight, .backBufferFormat = dimensions.outputFormat,
      .backBufferCount = 1u, .swapEffect = 1u, .windowed = 1u, .presentationInterval = 0x80000000u};
  const auto releaseFactory = [](D9CFactory* value) { if (value) dxmt9c_factory_release(value); };
  const auto releaseDevice = [](D9CDevice* value) { if (value) dxmt9c_device_release(value); };
  evidence.before = dxmt9::perf::snapshotRenderTapeReplay();
  evidence.phase = "fresh-device";
  std::unique_ptr<D9CFactory, decltype(releaseFactory)> factory(dxmt9c_factory_create(), releaseFactory);
  std::unique_ptr<D9CDevice, decltype(releaseDevice)> device(factory
      ? dxmt9c_factory_create_device(factory.get(), 0u, &params, 0u, nullptr) : nullptr, releaseDevice);
  evidence.deviceCreated = static_cast<bool>(device);
  if (device) {
    const auto upper = device->dev().upperDevice();
    evidence.actualMetal = upper && upper->pool();
    if (evidence.encodeDrawWitness) {
      require(evidence.actualMetal, "encode-draw-witness-requires-actual-metal");
      require(evidence.encodeDrawWitness->original.bindActualQueue(upper->queue()),
          evidence.encodeDrawWitness->original.contextFailure());
      const auto installFailure = dxmt9::RenderTapeEncodeDrawWitnessAccess::install(
          upper->queue(), evidence.encodeDrawWitness->recorder);
      require(!installFailure, installFailure);
      evidence.encodeDrawWitnessInstalled = true;
    }
    evidence.phase = "actual-replay";
    ++evidence.executionCount;
    const auto replayRoute = evidence.options.route == "production-slot"
        ? RenderTapePreparedRoute::ProductionSlot : RenderTapePreparedRoute::Compatibility;
    auto* replayObserver = evidence.encodeDrawWitness ? &evidence.encodeDrawWitness->original.observer() : nullptr;
    // The caller clock includes the complete return and assignment expression,
    // including callee locals and the existing result's replacement. It excludes
    // input/options preparation and the surrounding device/factory lifecycle.
    const auto providerCallStarted = std::chrono::steady_clock::now();
    evidence.result = replayPreparedRenderTapeIdentityV4(device.get(), prepared, replayRoute, replayObserver);
    const auto providerCallElapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - providerCallStarted).count();
    require(providerCallElapsed >= 0, "provider-call-clock-regressed");
    evidence.providerCallWallNanos = static_cast<std::uint64_t>(providerCallElapsed);
    evidence.providerCallReturned = true;
  } else evidence.result.status = FrameTapeReplayStatus::ObjectCreationFailed;
  evidence.phase = "actual-teardown";
  device.reset();
  factory.reset();
  evidence.teardownComplete = true;
  joinEncodeDrawIdentities(evidence);
  evidence.after = dxmt9::perf::snapshotRenderTapeReplay();
  evidence.countersObserved = true;
  evidence.countersMonotonic = monotonic(evidence.before, evidence.after);
  evidence.phase = "independent-qualification";
  evidence.query = queryQualified(general, evidence.preflight.queryReplay, evidence.result.queryReplay);
  evidence.sourceExact = evidence.result.validity.sourceReadback && input.source == evidence.result.sourcePixels;
  evidence.outputExact = evidence.result.validity.outputReadback && input.output == evidence.result.outputPixels;
  evidence.pixels = evidence.sourceExact && evidence.outputExact && evidence.result.validity.outputNonDegenerate &&
      evidence.result.validity.expectedDigestCaptured && evidence.result.validity.expectedDigestMatched &&
      evidence.result.validity.outputDigest == evidence.options.output &&
      evidence.result.validity.sourceDigest == evidence.options.source;
  const auto& v4 = evidence.result.exactDiagnosticV4.execution;
  const auto& setup = evidence.result.exactDiagnosticV4;
  const auto& conservation = evidence.result.conservation;
  const bool semantic = evidence.options.publicationMode == "semantic-mapping";
  evidence.semanticMappingQualified = semantic && v4.semanticMappingComplete &&
      v4.semanticMappingStatus == RenderTapeSemanticMappingV4Status::Valid && v4.semanticMapping &&
      v4.semanticMapping->valid() && v4.semanticMapping->planDigest() == evidence.options.plan &&
      v4.semanticMapping->records().size() == evidence.planRecords &&
      v4.semanticMapping->raws().size() == plan.identity().settlements.size() &&
      v4.sourceObligationsExpected == evidence.planSources &&
      v4.sourceObligationsRegistered == v4.sourceObligationsExpected &&
      v4.sourceObligationsQualified == v4.sourceObligationsExpected &&
      !v4.sourceObligationsPending && v4.semanticMapping->obligations().size() == evidence.planSources &&
      v4.sources.empty() && v4.initializers.empty() && v4.coordinators.empty();
  if (evidence.semanticMappingQualified) {
    std::map<std::uint32_t, std::set<std::uint32_t>> capturedGrouping, candidateGrouping;
    for (const auto& relation : v4.semanticMapping->commands()) {
      if (!relation.recordCount || relation.capturedCommandIndex == noIndex ||
          relation.candidateCommandIndex == noIndex) continue;
      capturedGrouping[relation.capturedCommandIndex].insert(relation.candidateCommandIndex);
      candidateGrouping[relation.candidateCommandIndex].insert(relation.capturedCommandIndex);
    }
    const auto changed = [](const auto& grouping) {
      return std::any_of(grouping.begin(), grouping.end(),
          [](const auto& row) { return row.second.size() > 1u; });
    };
    evidence.commandGroupingChanged = changed(capturedGrouping) || changed(candidateGrouping);
  }
  const bool presentPreparationQualified =
      v4.producerContract == producerContract(evidence.options) &&
      (evidence.options.producerContract == "none"
          ? !v4.presentPreparationAttempted && !v4.presentPreparationCompleted
          : v4.presentPreparationAttempted && v4.presentPreparationCompleted &&
            v4.presentPreparationHresult == 0 &&
            static_cast<std::uint64_t>(v4.presentPreparationEventIndex) + 1u ==
                plan.identity().header.presentOrdinal);
  const bool v4Complete = v4.validated && v4.armed && v4.complete && v4.qualifierOnly &&
      v4.publicationMode == publicationMode(evidence.options) &&
      presentPreparationQualified &&
      v4.canonicalTraversalComplete && v4.rawAckCoverageComplete && v4.actualAttemptsObserved &&
      v4.actualLifecycleComplete && v4.actualTopologyComplete &&
      (semantic ? evidence.semanticMappingQualified && !v4.exactSourceDiagnosticComplete
                : v4.exactSourceDiagnosticComplete && !v4.semanticMappingComplete && !v4.semanticMapping) &&
      !v4.policyQualified && !v4.queryReadinessQualified && !v4.pixelsQualified && !v4.timingEligible &&
      v4.queryBaselineExpected == evidence.preflight.queryReplay.checkpointDefinitions &&
      v4.queryBaselinesJoined == v4.queryBaselineExpected &&
      v4.queryObjectsMapped == evidence.preflight.queryReplay.queryDefinitions &&
      setup.initialSetupAdmitted && setup.initialSetupCompleted &&
      setup.initialSetupPrepared == setup.initialSetupOwners &&
      setup.initialSetupActivated == setup.initialSetupOwners &&
      setup.initialSetupConsumed == setup.initialSetupSeeds &&
      v4.dispatchedRecords == evidence.planRecords && v4.replayedEvents == evidence.planEvents &&
      v4.acknowledgedRaws == plan.identity().settlements.size() && v4.planDigest == evidence.options.plan;
  const bool routeObserved = evidence.countersMonotonic &&
      (evidence.after.directLeaseDraws > evidence.before.directLeaseDraws ||
       evidence.after.compatibilityDraws > evidence.before.compatibilityDraws) &&
      (evidence.options.route != "compatibility" ||
       (evidence.after.directLeaseDraws == evidence.before.directLeaseDraws &&
        evidence.after.directLeases == evidence.before.directLeases));
  evidence.qualified = evidence.result.complete() && evidence.actualMetal && v4Complete && evidence.query &&
      evidence.pixels && evidence.deviceCreated && evidence.teardownComplete && routeObserved &&
      evidence.after.encodeChunks > evidence.before.encodeChunks && evidence.after.gpuErrors == evidence.before.gpuErrors &&
      conservation.objectsCreated == conservation.objectsReleased &&
      conservation.inputBlobs == conservation.referencedBlobs && conservation.presentOrdinal && conservation.completionOrdinal &&
      evidence.result.coverage.presentRecords == 1u && evidence.result.coverage.presentSourceMappings == 1u &&
      evidence.result.coverage.presentOutputs == 1u && evidence.result.prepared.planReused &&
      evidence.result.prepared.immutableV4PreparationOwned && evidence.result.prepared.v4PreparationCount == 1u &&
      evidence.result.prepared.reusedImmutablePreparation && v4.admissionCount == 1u &&
      v4.preparedAdmissionConsumed;
  if (!evidence.qualified) {
    if (std::strcmp(evidence.result.exactDiagnosticV4.failure, "none") != 0)
      throw std::runtime_error(evidence.result.exactDiagnosticV4.failure);
    throw std::runtime_error("independent-qualification-rejected");
  }
  if (evidence.encodeDrawWitness) {
    const auto& collector = evidence.encodeDrawWitness->collector;
    const auto& original = evidence.encodeDrawWitness->original;
    const auto& observer = evidence.result.preparedObserverV4;
    evidence.phase = "encode-draw-witness";
    const bool complete = evidence.encodeDrawWitnessInstalled && !collector.failed() &&
        !collector.draws().empty() && !collector.issues().empty() && collector.fragmentBegins() &&
        collector.fragmentBegins() == collector.fragmentEnds() &&
        std::strcmp(evidence.encodeDrawJoinFailure, "none") == 0 &&
        observer.requested && observer.admitted && observer.complete && observer.closed &&
        std::strcmp(observer.failure, "none") == 0 && original.contextComplete() &&
        original.observationComplete() && original.receipts().size() == collector.issues().size();
    if (!complete) {
      evidence.qualified = false;
      throw std::runtime_error(collector.failed() ? witness::failureName(collector.failure())
          : std::strcmp(evidence.encodeDrawJoinFailure, "none") != 0
              ? evidence.encodeDrawJoinFailure : original.failed()
                  ? (std::strcmp(original.contextFailure(), "none") != 0
                      ? original.contextFailure() : original.sourceFailure())
                  : std::strcmp(observer.failure, "none") != 0
                      ? observer.failure : "encode-draw-witness-incomplete");
    }
  }
  evidence.phase = "complete";
}

void execute(Evidence& evidence) {
  DiagnosticPreparedInput session;
  prepareDiagnostic(evidence, session);
  if (!evidence.options.inspect) executeIteration(evidence, session);
}

const char* boolean(bool value) { return value ? "true" : "false"; }

void printDrawKey(std::ostream& out, witness::DrawKey key) {
  out << "{\"sequence\":" << key.sequence << ",\"command_index\":" << key.command
      << ",\"subdraw\":" << key.subdraw << '}';
}

void printOriginalBufferIdentity(std::ostream& out, D9CWireObjectIdentity identity) {
  if (!identity.kind || !identity.generation || !identity.objectId) {
    out << "null";
    return;
  }
  out << "{\"kind\":" << identity.kind << ",\"generation\":" << identity.generation
      << ",\"object_id\":" << identity.objectId << '}';
}

void printOriginalEncodeWitness(std::ostream& out, const Evidence& evidence) {
  // Called only after execute returned/unwound all local device owners. No
  // producer-only status or Encode-only receipt is read while callbacks live.
  const auto& observer = evidence.result.preparedObserverV4;
  out << ",\"original_buffer_observer\":{\"requested\":" << boolean(observer.requested)
      << ",\"admitted\":" << boolean(observer.admitted) << ",\"complete\":" << boolean(observer.complete)
      << ",\"closed\":" << boolean(observer.closed) << ",\"failure\":" << quoted(observer.failure)
      << ",\"buffer_callbacks\":" << observer.bufferCallbacks << ",\"buffer_phases\":[";
  for (std::size_t index = 0u; index < observer.bufferPhases.size(); ++index) {
    if (index) out << ',';
    out << observer.bufferPhases[index];
  }
  out << "],\"before_record_callbacks\":" << observer.beforeRecordCallbacks
      << ",\"armed_callbacks\":" << observer.armedCallbacks
      << ",\"closed_callbacks\":" << observer.closedCallbacks
      << ",\"association_raws\":" << observer.associationRaws
      << ",\"handle_associations\":" << observer.handleAssociations << '}';
  if (!evidence.encodeDrawWitness) {
    out << ",\"original_context\":{\"complete\":false,\"failure\":\"not-created\"}"
        << ",\"original_binding_proof\":{\"requested\":" << boolean(evidence.options.observeEncodeDraws)
        << ",\"observation_complete\":false,\"source_origin_failure\":\"not-created\",\"qualified_draws\":0}";
    return;
  }
  const auto& original = evidence.encodeDrawWitness->original;
  const auto budget = original.expectedBudget();
  const auto receipts = original.receipts();
  const bool observationComplete = evidence.teardownComplete && original.observationComplete() &&
      receipts.size() == evidence.encodeDrawWitness->collector.issues().size();
  constexpr std::size_t rowLimit = 4096u;
  constexpr auto statusCount = static_cast<std::size_t>(dxmt9::test::OriginalEncodeDirectStatus::PhysicalByteProofRejected) + 1u;
  std::array<std::size_t, statusCount> statuses{};
  std::size_t qualified = 0u;
  std::uint64_t plannedBytes = 0u, attemptedReadBytes = 0u, provedBytes = 0u;
  for (const auto& receipt : receipts) {
    const auto& row = receipt.observation;
    ++statuses[static_cast<std::size_t>(row.status)];
    plannedBytes += row.plannedReadBytes;
    attemptedReadBytes += row.attemptedInspectionBytes / 2u;
    if (row.qualified()) {
      ++qualified;
      if (row.readProof) provedBytes += row.readProof->provedBytes;
    }
  }
  out << ",\"original_context\":{\"complete\":" << boolean(evidence.teardownComplete && original.contextComplete())
      << ",\"failure\":" << quoted(original.contextFailure())
      << ",\"status\":" << static_cast<unsigned>(original.contextStatus())
      << ",\"record_contexts\":" << original.recordCount()
      << ",\"outside_body_record_callbacks\":" << original.outsideBodyRecordCallbacks()
      << ",\"expected_byte_limit\":" << budget.maxRetainedExpectedBufferBytes
      << ",\"expected_bytes_charged\":" << budget.chargedExpectedBufferBytes
      << ",\"expected_bytes_high_water\":" << budget.highWaterExpectedBufferBytes
      << ",\"scratch_byte_limit\":67108864,\"expected_source\":\"authenticated-input-blobs-only\"}"
      << ",\"original_binding_proof\":{\"requested\":" << boolean(evidence.options.observeEncodeDraws)
      << ",\"observation_complete\":" << boolean(observationComplete)
      << ",\"source_origin_failure\":" << quoted(original.sourceFailure())
      << ",\"scope\":\"single-stream-frequency1-instance1-noUP-original-source-row-contract\""
      << ",\"equality_scope\":\"coherent-instantaneous-selected-IB-VB-footprints-under-one-arena-lock\""
      << ",\"address_scope\":\"actual-selected-snapshots-final-native-bindings-and-current-slot5-volatile\""
      << ",\"read_credit_scope\":\"pre-admitted-attempted-bytes-not-measured-physical-reads\""
      << ",\"full_semantic_qualified\":false,\"full_shader_fetch_coverage_qualified\":false"
      << ",\"policy_qualified\":false,\"timing_eligible\":false,\"future_preservation_qualified\":false"
      << ",\"raw_mapped_writes_qualified\":false,\"gpu_writes_qualified\":false"
      << ",\"source_fragments\":" << original.sourceFragments()
      << ",\"setup_source_fragments\":" << original.setupSourceFragments()
      << ",\"teardown_source_fragments\":" << original.teardownSourceFragments()
      << ",\"setup_draw_attempts\":" << original.setupDrawAttempts()
      << ",\"teardown_draw_attempts\":" << original.teardownDrawAttempts()
      << ",\"issued_receipts\":" << receipts.size() << ",\"qualified_draws\":" << qualified
      << ",\"unqualified_draws\":" << receipts.size() - qualified
      << ",\"all_issued_draws_qualified\":" << boolean(observationComplete && qualified == receipts.size())
      << ",\"planned_read_bytes\":" << plannedBytes
      << ",\"attempted_read_bytes\":" << attemptedReadBytes
      << ",\"attempted_inspection_bytes\":" << original.attemptedInspectionBytes()
      << ",\"proved_bytes\":" << provedBytes
      << ",\"limits\":{\"source_rows\":32768,\"ranges\":32769,\"issue_read_bytes\":67108864"
      << ",\"aggregate_inspection_bytes\":1073741824,\"issued_receipts\":65536}"
      << ",\"status_counts\":{ ";
  for (std::size_t index = 0u; index < statuses.size(); ++index) {
    if (index) out << ',';
    out << quoted(dxmt9::test::originalEncodeDirectStatusName(
        static_cast<dxmt9::test::OriginalEncodeDirectStatus>(index))) << ':' << statuses[index];
  }
  out << "},\"row_limit\":" << rowLimit << ",\"rows_truncated\":" << boolean(receipts.size() > rowLimit)
      << ",\"draws\":[";
  for (std::size_t index = 0u; index < std::min(rowLimit, receipts.size()); ++index) {
    if (index) out << ',';
    const auto& receipt = receipts[index];
    const auto& row = receipt.observation;
    out << "{\"issue\":" << index << ",\"draw\":";
    printDrawKey(out, receipt.draw);
    out << ",\"original_event\":" << receipt.original.eventOrdinal
        << ",\"original_record\":" << receipt.original.recordIndex
        << ",\"source_index\":" << receipt.sourceIndex << ",\"source_ordinal\":" << receipt.sourceOrdinal
        << ",\"vertex_identity\":";
    printOriginalBufferIdentity(out, receipt.vertexIdentity);
    out << ",\"index_identity\":";
    printOriginalBufferIdentity(out, receipt.indexIdentity);
    out << ",\"vertex_content_epoch\":" << receipt.vertexEpoch
        << ",\"index_content_epoch\":" << receipt.indexEpoch
        << ",\"vertex_buffer_observation_id\":" << receipt.vertexObservationId
        << ",\"index_buffer_observation_id\":" << receipt.indexObservationId
        << ",\"source_base\":" << receipt.sourceBase << ",\"source_stride\":" << receipt.sourceStride
        << ",\"draw_volatile\":";
    if (!receipt.drawVolatile) out << "null";
    else {
      const auto& vol = *receipt.drawVolatile;
      const bool divisorsZero = std::all_of(vol.streamInstanceDivisors.begin(), vol.streamInstanceDivisors.end(),
          [](std::uint32_t divisor) { return divisor == 0u; });
      out << "{\"vertex_base_index\":" << vol.vertexBaseIndex
          << ",\"vertex_stream_offset\":" << vol.vertexStreamOffset
          << ",\"vertex_stream_stride\":" << vol.vertexStreamStride << ",\"pad\":" << vol._pad
          << ",\"instance_divisors_all_zero\":" << boolean(divisorsZero) << '}';
    }
    out << ",\"qualified\":" << boolean(row.qualified())
        << ",\"status\":" << quoted(dxmt9::test::originalEncodeDirectStatusName(row.status))
        << ",\"status_code\":" << static_cast<unsigned>(row.status)
        << ",\"source_row_status_code\":";
    if (row.plannerStatus) out << static_cast<unsigned>(*row.plannerStatus); else out << "null";
    out << ",\"address_status\":";
    if (row.addressStatus) out << quoted(dxmt9::test::originalEncodeAddressStatusName(*row.addressStatus));
    else out << "null";
    out << ",\"planned_read_bytes\":" << row.plannedReadBytes
        << ",\"attempted_inspection_bytes\":" << row.attemptedInspectionBytes
        << ",\"unique_vertex_rows\":" << row.uniqueVertexRows << ",\"vertex_ranges\":" << row.vertexRanges
        << ",\"read_proof\":";
    if (!row.readProof) out << "null";
    else {
      const auto& proof = *row.readProof;
      out << "{\"qualified\":" << boolean(proof.proved())
          << ",\"status_code\":" << static_cast<unsigned>(proof.status) << ",\"failed_request\":";
      if (proof.failedRequest == noIndex) out << "null"; else out << proof.failedRequest;
      out << ",\"failed_range\":";
      if (proof.failedRange == noIndex) out << "null"; else out << proof.failedRange;
      out << ",\"proved_requests\":" << proof.provedRequests
          << ",\"proved_ranges\":" << proof.provedRanges << ",\"proved_bytes\":" << proof.provedBytes << '}';
    }
    out << '}';
  }
  out << "]}";
}

void printEncodeDrawWitness(std::ostream& out, const Evidence& evidence) {
  out << "{\"requested\":" << boolean(evidence.options.observeEncodeDraws)
      << ",\"installed\":" << boolean(evidence.encodeDrawWitnessInstalled)
      << ",\"suppression_flags_all_false\":" << boolean(evidence.encodeDrawWitness &&
          !evidence.encodeDrawWitness->recorder.suppressMetalCalls &&
          !evidence.encodeDrawWitness->recorder.suppressCommandEncoderSideEffects &&
          !evidence.encodeDrawWitness->recorder.suppressBaseStateLookup)
      << ",\"full_semantic_qualified\":false,\"policy_qualified\":false,\"timing_eligible\":false"
      << ",\"scope\":\"upload-input-destination-and-native-bindings-with-separate-scoped-original-buffer-proof\""
      << ",\"fingerprint_scope\":\"fnv1a64-observation-not-collision-free-semantic-proof\""
      << ",\"selection_scope\":\"bound-stream-slots-not-reflected-shader-consumption\""
      << ",\"native_identity_scope\":\"run-local-repeated-native-handle-values-not-lifetime-content-or-cross-run-authority\""
      << ",\"direct_binding_byte_equality_qualified\":false"
      << ",\"future_preservation_qualified\":false";
  out << ",\"encoding_domain\":\"serial-only\",\"parallel_domain_supported\":false";
  printOriginalEncodeWitness(out, evidence);
  if (!evidence.encodeDrawWitness) { out << '}'; return; }
  const auto& collector = evidence.encodeDrawWitness->collector;
  const auto& limits = collector.limits();
  const auto uploads = collector.uploads();
  const auto reuses = collector.reuses();
  const auto issues = collector.issues();
  constexpr std::size_t rowLimit = 4096u;
  const bool complete = evidence.encodeDrawWitnessInstalled && evidence.teardownComplete &&
      !collector.failed() && !collector.draws().empty() && !issues.empty() &&
      collector.fragmentBegins() && collector.fragmentBegins() == collector.fragmentEnds() &&
      std::strcmp(evidence.encodeDrawJoinFailure, "none") == 0 &&
      evidence.result.preparedObserverV4.requested && evidence.result.preparedObserverV4.admitted &&
      evidence.result.preparedObserverV4.complete && evidence.result.preparedObserverV4.closed &&
      std::strcmp(evidence.result.preparedObserverV4.failure, "none") == 0 &&
      evidence.encodeDrawWitness->original.contextComplete() &&
      evidence.encodeDrawWitness->original.observationComplete() &&
      evidence.encodeDrawWitness->original.receipts().size() == issues.size();
  std::size_t compared = 0u, selected = 0u, expandedMainSelected = 0u;
  std::size_t boundStreams = 0u, streamsWithoutUpload = 0u;
  std::size_t boundIndices = 0u, indicesWithoutUpload = 0u;
  std::size_t joinedSources = 0u, completedSources = 0u, joinedOrigins = 0u;
  for (const auto& joined : evidence.encodeDrawJoins) {
    if (joined.sourceIndex != noIndex) ++joinedSources;
    if (joined.sourceComplete) ++completedSources;
    if (joined.originalMatches == 1u) ++joinedOrigins;
  }
  for (const auto& upload : uploads) {
    if (upload.destinationCompared) ++compared;
    if (upload.selectedIssues) {
      ++selected;
      if (upload.kind == witness::BufferKind::TransientVertexUpload && upload.reason == 3u)
        ++expandedMainSelected;
    }
  }
  for (const auto& issue : issues) {
    boundStreams += issue.boundStreamCount;
    streamsWithoutUpload += issue.boundStreamCount - issue.selectedStreamCount;
    if (issue.indexed) {
      ++boundIndices;
      if (issue.indexUpload == noIndex) ++indicesWithoutUpload;
    }
  }
  // Process-local native handles remain internal comparison keys. Export
  // one bounded run-local ID namespace for input and final native bindings,
  // never a native pointer or dereference key. IDs imply no backing lifetime.
  out << ",\"complete\":" << boolean(complete)
      << ",\"failure\":" << quoted(witness::failureName(collector.failure()))
      << ",\"destination_failure_status\":" << static_cast<unsigned>(collector.destinationFailureStatus())
      << ",\"identity_join_failure\":" << quoted(evidence.encodeDrawJoinFailure)
      << ",\"limits\":{\"draws\":" << limits.maxDraws << ",\"uploads\":" << limits.maxUploads
      << ",\"reuses\":" << limits.maxReuses << ",\"issues\":" << limits.maxIssues
      << ",\"native_buffers\":" << limits.maxNativeBuffers
      << ",\"member_bytes\":" << limits.maxMemberBytes << ",\"aggregate_inspection_bytes\":" << limits.maxObservedBytes
      << ",\"hard_rows\":65536,\"hard_member_bytes\":67108864,\"hard_aggregate_inspection_bytes\":1073741824}"
      << ",\"source_fragment_begins\":" << collector.fragmentBegins()
      << ",\"source_fragment_ends\":" << collector.fragmentEnds()
      << ",\"draw_begins\":" << collector.draws().size() << ",\"draw_issues\":" << issues.size()
      << ",\"native_buffers_observed\":" << collector.nativeBufferCount()
      << ",\"issued_native_stream_bindings\":" << boundStreams
      << ",\"issued_stream_bindings_without_upload_receipt\":" << streamsWithoutUpload
      << ",\"issued_native_index_bindings\":" << boundIndices
      << ",\"issued_index_bindings_without_upload_receipt\":" << indicesWithoutUpload
      << ",\"successful_uploads\":" << uploads.size() << ",\"cache_reuses\":" << reuses.size()
      << ",\"orphan_uploads\":" << collector.orphanUploads() << ",\"issued_selected_uploads\":" << selected
      << ",\"issued_selected_expanded_main_uploads\":" << expandedMainSelected
      << ",\"destination_compared_uploads\":" << compared
      << ",\"observed_input_only_uploads\":" << uploads.size() - compared
      << ",\"inspection_bytes\":" << collector.observedBytes()
      << ",\"source_identity_joined_issues\":" << joinedSources
      << ",\"source_completion_joined_issues\":" << completedSources
      << ",\"original_record_joined_issues\":" << joinedOrigins
      << ",\"original_record_mapping_complete\":" << boolean(!issues.empty() && joinedOrigins == issues.size())
      << ",\"row_limit_each\":" << rowLimit << ",\"rows_truncated\":" << boolean(
          uploads.size() > rowLimit || reuses.size() > rowLimit || issues.size() > rowLimit)
      << ",\"uploads\":[";
  for (std::size_t index = 0u; index < std::min(rowLimit, uploads.size()); ++index) {
    if (index) out << ',';
    const auto& row = uploads[index];
    out << "{\"receipt\":" << index << ",\"draw\":";
    printDrawKey(out, row.draw);
    out << ",\"kind\":" << static_cast<unsigned>(row.kind)
        << ",\"reason\":" << static_cast<unsigned>(row.reason) << ",\"logical_source_handle\":" << row.sourceHandle
        << ",\"buffer_observation_id\":" << row.bufferObservationId
        << ",\"offset\":" << row.slice.offset << ",\"bytes\":" << row.slice.size
        << ",\"input_fingerprint_fnv1a64\":" << row.inputDigest
        << ",\"instantaneous_destination_equal\":" << boolean(row.destinationCompared)
        << ",\"destination_status\":" << static_cast<unsigned>(row.destinationStatus)
        << ",\"selected_issues\":" << row.selectedIssues << '}';
  }
  out << "],\"reuses\":[";
  for (std::size_t index = 0u; index < std::min(rowLimit, reuses.size()); ++index) {
    if (index) out << ',';
    const auto& row = reuses[index];
    out << "{\"draw\":";
    printDrawKey(out, row.draw);
    out << ",\"kind\":" << static_cast<unsigned>(row.kind) << ",\"original_upload_receipt\":" << row.upload << '}';
  }
  out << "],\"issues\":[";
  for (std::size_t index = 0u; index < std::min(rowLimit, issues.size()); ++index) {
    if (index) out << ',';
    const auto& row = issues[index];
    out << "{\"issue\":" << index << ",\"draw\":";
    printDrawKey(out, row.draw);
    out << ",\"indexed\":" << boolean(row.indexed) << ",\"primitive_type\":" << row.primitiveType
        << ",\"count\":" << row.count << ",\"vertex_start\":" << row.vertexStart
        << ",\"instances\":" << row.instanceCount << ",\"base_vertex\":" << row.baseVertex
        << ",\"base_instance\":" << row.baseInstance << ",\"selected_index_upload_receipt\":";
    if (row.indexUpload == noIndex) out << "null"; else out << row.indexUpload;
    out << ",\"issued_index_buffer_observation_id\":" << row.indexBufferObservationId
        << ",\"issued_index_offset\":" << row.indexSlice.offset
        << ",\"issued_index_bytes\":" << row.indexSlice.size
        << ",\"bound_stream_count\":" << static_cast<unsigned>(row.boundStreamCount)
        << ",\"upload_selected_stream_count\":" << static_cast<unsigned>(row.selectedStreamCount)
        << ",\"bound_streams\":[";
    for (std::size_t stream = 0u; stream < row.boundStreamCount; ++stream) {
      if (stream) out << ',';
      const auto& binding = row.streams[stream];
      out << "{\"slot\":" << static_cast<unsigned>(binding.slot)
          << ",\"buffer_observation_id\":" << binding.bufferObservationId
          << ",\"binding_offset\":" << binding.bindingOffset << ",\"upload_receipt\":";
      if (binding.upload == noIndex) out << "null"; else out << binding.upload;
      out << '}';
    }
    out << "],\"selected_streams\":[";
    std::size_t reportedSelected = 0u;
    for (std::size_t stream = 0u; stream < row.boundStreamCount; ++stream) {
      const auto& binding = row.streams[stream];
      if (binding.upload == noIndex) continue;
      if (reportedSelected++) out << ',';
      out << "{\"slot\":" << static_cast<unsigned>(binding.slot)
          << ",\"upload_receipt\":" << binding.upload
          << ",\"buffer_observation_id\":" << binding.bufferObservationId
          << ",\"binding_offset\":" << binding.bindingOffset << '}';
    }
    out << "],\"identity\":";
    if (index >= evidence.encodeDrawJoins.size()) out << "null";
    else {
      const auto& joined = evidence.encodeDrawJoins[index];
      out << "{\"source_known\":" << boolean(joined.sourceIndex != noIndex)
          << ",\"source_index\":";
      if (joined.sourceIndex == noIndex) out << "null"; else out << joined.sourceIndex;
      out << ",\"source_ordinal\":" << joined.sourceOrdinal << ",\"local_raw_ordinal\":" << joined.rawOrdinal
          << ",\"writer_generation\":" << joined.writerGeneration << ",\"storage_generation\":" << joined.storageGeneration
          << ",\"source_completion_observed\":" << boolean(joined.sourceComplete)
          << ",\"original_match_count\":" << joined.originalMatches
          << ",\"original_record_mapping\":" << quoted(joined.originalMatches == 1u ? "authenticated-semantic-map"
              : joined.originalMatches ? "ambiguous-not-qualified" : "missing-not-inferred")
          << ",\"original_event\":";
      if (joined.originalMatches == 1u) out << joined.originalEvent; else out << "null";
      out << ",\"original_record\":";
      if (joined.originalMatches == 1u) out << joined.originalRecord; else out << "null";
      out << ",\"original_type\":";
      if (joined.originalMatches == 1u) out << joined.originalType; else out << "null";
      out << '}';
    }
    out << '}';
  }
  out << "]}";
}

void printReadbackArtifact(std::ostream& out, const ReadbackArtifact& artifact) {
  out << "{\"attempted\":" << boolean(artifact.attempted)
      << ",\"written\":" << boolean(artifact.written) << ",\"bytes\":" << artifact.bytes
      << ",\"written_bytes\":" << artifact.writtenBytes << ",\"sha256\":";
  if (artifact.digestComputed) out << quoted(hexBytes(artifact.digest));
  else out << "null";
  out << ",\"failure\":" << quoted(artifact.failure) << '}';
}

void printReadbackArtifacts(std::ostream& out, const Evidence& evidence) {
  const auto& artifacts = evidence.readbacks;
  out << "{\"requested\":" << boolean(artifacts.requested)
      << ",\"attempted\":" << boolean(artifacts.attempted)
      << ",\"directory\":" << quoted(evidence.options.readbackDirectory.string())
      << ",\"source\":";
  printReadbackArtifact(out, artifacts.source);
  out << ",\"output\":";
  printReadbackArtifact(out, artifacts.output);
  out << ",\"failure\":" << quoted(artifacts.failure) << '}';
}

void printQuery(std::ostream& out, const RenderTapeQueryReplayEvidence& query) {
  out << "{\"present\":" << boolean(query.present) << ",\"actual_metal\":" << boolean(query.actualMetal)
      << ",\"transcript_complete\":" << boolean(query.transcriptComplete) << ",\"oracle_complete\":" << boolean(query.oracleComplete)
      << ",\"gpu_result_oracle\":" << boolean(query.gpuResultOracle) << ",\"failure_code\":" << static_cast<unsigned>(query.failure)
      << ",\"progress_limit_code\":" << static_cast<unsigned>(query.progressLimit)
      << ",\"query_definitions\":" << query.queryDefinitions << ",\"issue_records\":" << query.issueRecords
      << ",\"replayed_issue_records\":" << query.replayedIssueRecords << ",\"recorded_polls\":" << query.recordedPolls
      << ",\"replayed_polls\":" << query.replayedPolls << ",\"captured_ready\":" << query.capturedReady
      << ",\"captured_pending\":" << query.capturedPending << ",\"replay_ready\":" << query.replayReady
      << ",\"replay_pending\":" << query.replayPending << ",\"readiness_required\":" << query.readinessRequired
      << ",\"readiness_satisfied\":" << query.readinessSatisfied << ",\"required_results\":" << query.requiredResults
      << ",\"matched_results\":" << query.matchedResults << ",\"substituted_results\":" << query.substitutedResults
      << ",\"supplemental_polls\":" << query.supplementalPolls << ",\"supplemental_drains\":" << query.supplementalDrains
      << ",\"checkpoint_definitions\":" << query.checkpointDefinitions << ",\"checkpoint_completed\":" << query.checkpointCompleted
      << ",\"checkpoint_activated\":" << query.checkpointActivated << ",\"rows\":[";
  for (std::size_t index = 0u; index < query.rows.size(); ++index) {
    if (index) out << ',';
    const auto& row = query.rows[index];
    const auto bytes = std::min<std::size_t>(row.definedBytes, row.capturedBytes.size());
    out << "{\"event_index\":" << row.eventIndex << ",\"event_ordinal\":" << row.eventOrdinal
        << ",\"query_kind\":" << row.identity.kind << ",\"query_generation\":" << row.identity.generation
        << ",\"query_object_id\":" << row.identity.objectId << ",\"issue_cycle\":" << row.issueCycle
        << ",\"flags\":" << row.flags << ",\"has_output\":" << boolean(row.hasOutput) << ",\"data_size\":" << row.dataSize
        << ",\"captured_result\":" << row.capturedResult << ",\"replay_result\":" << row.replayResult
        << ",\"oracle_result\":" << row.oracleResult << ",\"defined_bytes\":" << row.definedBytes
        << ",\"captured_hex\":" << quoted(hexBytes(std::span(row.capturedBytes).first(bytes)))
        << ",\"replay_hex\":" << quoted(hexBytes(std::span(row.replayBytes).first(bytes)))
        << ",\"result_required\":" << boolean(row.resultRequired) << ",\"result_matched\":" << boolean(row.resultMatched)
        << ",\"readiness_satisfied\":" << boolean(row.readinessSatisfied) << ",\"result_substituted\":" << boolean(row.resultSubstituted)
        << ",\"local_query_identity\":" << row.localQueryIdentity << ",\"local_issue_ordinal\":" << row.localIssueOrdinal
        << ",\"local_device_epoch\":" << row.localDeviceEpoch << ",\"local_queue\":" << row.localQueue
        << ",\"local_sequence\":" << row.localSequence << ",\"local_initializer\":" << row.localInitializer
        << ",\"completed_sequence\":" << row.completedSequence << ",\"completed_initializer\":" << row.completedInitializer
        << ",\"oracle_polls\":" << row.oraclePolls << ",\"supplemental\":" << boolean(row.supplemental) << '}';
  }
  out << "]}";
}

template <typename Row>
void printPublicationCut(std::ostream& out, const Row& row) {
  out << "{\"event_ordinal\":" << row.cutEventOrdinal
      << ",\"cut_kind\":" << row.cutKind << ",\"cut_record\":" << row.cutRecord
      << ",\"publication_edge\":" << row.publicationEdge
      << ",\"submission_edge\":" << row.submissionEdge
      << ",\"completion_edge\":" << row.completionEdge;
  if constexpr (requires { row.commandCount; })
    out << ",\"command_count\":" << row.commandCount << ",\"sequence\":" << row.seqId;
  else
    out << ",\"sequence\":" << row.sequence;
  out << "}";
}

void printPublicationCuts(std::ostream& out, const Evidence& evidence) {
  const auto& actual = evidence.result.exactDiagnosticV4.execution.actualIdentity;
  const auto sources = std::min(std::size_t{8u}, actual.sources.size());
  const auto initializers = std::min(std::size_t{8u}, actual.initializers.size());
  out << "{\"row_limit\":8,\"truncated\":" << boolean(
      evidence.planSources > 8u || evidence.planInitializers > 8u ||
      actual.sources.size() > 8u || actual.initializers.size() > 8u)
      << ",\"expected_sources\":[";
  for (std::size_t i = 0u; i < evidence.reportedSources; ++i) {
    if (i) out << ',';
    printPublicationCut(out, evidence.expectedSources[i]);
  }
  out << "],\"actual_sources\":[";
  for (std::size_t i = 0u; i < sources; ++i) {
    if (i) out << ',';
    printPublicationCut(out, actual.sources[i].lifecycle);
  }
  out << "],\"expected_initializers\":[";
  for (std::size_t i = 0u; i < evidence.reportedInitializers; ++i) {
    if (i) out << ',';
    printPublicationCut(out, evidence.expectedInitializers[i]);
  }
  out << "],\"actual_initializers\":[";
  for (std::size_t i = 0u; i < initializers; ++i) {
    if (i) out << ',';
    printPublicationCut(out, actual.initializers[i]);
  }
  out << "]}";
}

void printTimingMeasurements(std::ostream& out, const Evidence& evidence) {
  const auto& prepared = evidence.result.prepared;
  // These are existing elapsed service scopes, not a new performance gate.
  // Failed/inspection receipts cannot turn unobserved phases into zero work.
  const bool replayQualified = !evidence.options.inspect && evidence.failure.empty() &&
      evidence.qualified && evidence.actualMetal && evidence.teardownComplete &&
      evidence.executionCount == 1u;
  const bool preparedObserved = replayQualified && prepared.immutableInputOwned &&
      prepared.preflightCompleted && prepared.planReused;
  const bool snapshotObserved = replayQualified && evidence.countersObserved;
  const bool snapshotMonotonic = snapshotObserved && evidence.countersMonotonic;
  const auto printNanos = [&](const char* name, bool observed, std::uint64_t value) {
    out << ',' << quoted(name) << ':';
    if (observed) out << value; else out << "null";
  };
  const auto printDeltaNanos = [&](const char* name, std::uint64_t before, std::uint64_t after) {
    out << ',' << quoted(name) << ':';
    if (snapshotMonotonic) out << after - before; else out << "null";
  };
  out << "{\"schema\":\"dxmt9.render_tape.v4_diagnostic_timing.v2\""
      << ",\"authority\":\"diagnostic-service-cost-only\""
      << ",\"clock\":\"std::chrono::steady_clock\",\"units\":\"nanoseconds\""
      << ",\"thread_cpu_time\":false,\"additive\":false,\"overlapping_scopes\":true"
      << ",\"prepared_phases\":{\"observed\":" << boolean(preparedObserved)
      << ",\"scope\":\"owned-input-preparation-and-prepared-replay-phases;"
         "excludes-file-loading-factory-creation-device-teardown\""
      << ",\"zero_semantics\":\"not-entered-or-below-clock-resolution\""
      << ",\"prepare_wall_scope\":\"owned-tape-blob-copy-general-provider-preflight-"
         "and-immutable-v4-preparation\""
      << ",\"replay_wall_scope\":\"replay-general-plan-after-policy-admission-and-reset;"
         "not-entire-provider-call\""
      << ",\"resource_setup_scope\":\"object-bootstrap-and-mutations-including-synchronous-drains\""
      << ",\"chunk_replay_scope\":\"provider-import-resolve-dispatch-including-synchronous-drains;"
         "not-encode-thread\""
      << ",\"readback_drain_scope\":\"flush-completion-readback-format-packing-digest;not-gpu-only\""
      << ",\"cleanup_scope\":\"provider-flush-and-release;not-device-teardown\""
      << ",\"reset_scope\":\"fresh-device-no-warm-reset\"";
  const auto& timing = prepared.timing;
  printNanos("prepare_wall_ns", preparedObserved, timing.prepareWallNanos);
  printNanos("reset_wall_ns", preparedObserved, timing.resetWallNanos);
  printNanos("resource_setup_ns", preparedObserved, timing.resourceSetupNanos);
  printNanos("chunk_replay_ns", preparedObserved, timing.chunkReplayNanos);
  printNanos("readback_drain_ns", preparedObserved, timing.readbackDrainNanos);
  printNanos("cleanup_ns", preparedObserved, timing.cleanupNanos);
  printNanos("replay_wall_ns", preparedObserved, timing.replayWallNanos);
  out << "},\"runtime_snapshot_delta\":{\"observed\":" << boolean(snapshotObserved)
      << ",\"monotonic\":" << boolean(snapshotMonotonic)
      << ",\"scope\":\"before-fresh-factory-create-through-after-device-factory-teardown\""
      << ",\"zero_semantics\":\"no-recorded-interval;"
         "gated-counter-zero-does-not-prove-no-work\""
      << ",\"prepared_serial_build_gate\":\"DXMT9_PREPARED_BUILD_TIMING=1-required\""
      << ",\"resource_mark_scope\":\"nested-in-prepare-slot\""
      << ",\"prepared_serial_build_scope\":\"prepared-serial-read-planning-plus-resource-marking;"
         "not-framegraph-planning\""
      << ",\"encode_chunk_scope\":\"actual-encode-thread-inclusive-render-pass-setup-pso-work-"
         "and-metal-encoding;not-encoder-only\"";
  printDeltaNanos("prepare_slot_wall_ns", evidence.before.prepareSlotWallNs,
      evidence.after.prepareSlotWallNs);
  printDeltaNanos("resource_mark_wall_ns", evidence.before.resourceMarkWallNs,
      evidence.after.resourceMarkWallNs);
  printDeltaNanos("prepared_serial_build_wall_ns", evidence.before.preparedSerialBuildWallNs,
      evidence.after.preparedSerialBuildWallNs);
  printDeltaNanos("encode_chunk_wall_ns", evidence.before.encodeChunkWallNs,
      evidence.after.encodeChunkWallNs);
  // No existing snapshot exposes exclusive planning, pass preparation, GPU
  // duration, completion wait, or readback. Inclusive intervals cannot supply
  // those values by subtraction, especially across asynchronous stages.
  out << "},\"unavailable\":{\"framegraph_planning_ns\":null"
      << ",\"render_pass_preparation_ns\":null,\"encoder_only_ns\":null"
      << ",\"gpu_execution_ns\":null,\"gpu_completion_wait_ns\":null"
      << ",\"readback_only_ns\":null}}";
}

void printQueryTimingMeasurements(std::ostream& out, const Evidence& evidence) {
  const auto& query = evidence.result.queryReplay;
  // Export existing elapsed service scopes only. A query-free or unqualified
  // replay cannot turn missing observations into zero query work.
  const bool observed = !evidence.options.inspect && evidence.failure.empty() &&
      evidence.qualified && evidence.actualMetal && evidence.deviceCreated &&
      evidence.teardownComplete && evidence.executionCount == 1u && query.present &&
      query.actualMetal && query.transcriptComplete && query.oracleComplete;
  const auto printValue = [&](const char* name, std::uint64_t value) {
    out << ',' << quoted(name) << ':';
    if (observed) out << value; else out << "null";
  };
  out << "{\"schema\":\"dxmt9.render_tape.v4_query_timing.v1\""
      << ",\"authority\":\"diagnostic-service-cost-only\""
      << ",\"clock\":\"std::chrono::steady_clock\",\"units\":\"nanoseconds\""
      << ",\"thread_cpu_time\":false,\"additive\":false,\"observed\":" << boolean(observed)
      << ",\"checkpoint_setup_scope\":\"checkpoint-event-create-end-snapshot-flush-poll-yield-"
         "and-receipt-bookkeeping;not-gpu-only\""
      << ",\"transcript_scope\":\"captured-query-getdata-validation-dispatch-including-flush-"
         "snapshot-result-comparison-and-row-bookkeeping;excludes-query-issue-records\""
      << ",\"oracle_scope\":\"post-transcript-deferred-query-obligation-snapshot-validation-"
         "promotion-flush-poll-yield-and-result-comparison;not-gpu-only\""
      << ",\"nesting_scope\":\"within-prepared-replay-wall;not-additive-with-other-service-scopes\""
      << ",\"checkpoint_setup_count_scope\":\"setup-end-calls-queue-publication-deltas-poll-calls-"
         "and-pending-poll-yield-retries;not-blocking-wait-time\""
      << ",\"zero_semantics\":\"no-recorded-interval-or-below-clock-resolution;"
         "not-proof-of-no-query-work\""
      << ",\"null_semantics\":\"unobserved-or-unqualified\"";
  printValue("checkpoint_setup_wall_ns", query.checkpointSetupWallNanos);
  printValue("transcript_wall_ns", query.transcriptWallNanos);
  printValue("oracle_wall_ns", query.oracleWallNanos);
  printValue("checkpoint_setup_end_count", query.checkpointSetupEnds);
  printValue("checkpoint_setup_queue_publication_count", query.checkpointSetupPublications);
  printValue("checkpoint_setup_poll_count", query.checkpointSetupPolls);
  printValue("checkpoint_setup_pending_poll_yield_count", query.checkpointSetupWaits);
  out << '}';
}

void printV4PhaseTimingMeasurements(std::ostream& out, const Evidence& evidence) {
  const auto& timing = evidence.result.prepared.timing;
  // The outer phase precedes replayWallNanos; the other phases are nested in
  // it. These diagnostic service scopes must not be added to inclusive costs.
  const bool observed = !evidence.options.inspect && evidence.failure.empty() &&
      evidence.qualified && evidence.actualMetal && evidence.deviceCreated &&
      evidence.teardownComplete && evidence.executionCount == 1u;
  const auto printNanos = [&](const char* name, std::uint64_t value) {
    out << ',' << quoted(name) << ':';
    if (observed) out << value; else out << "null";
  };
  out << "{\"schema\":\"dxmt9.render_tape.v4_phase_timing.v2\""
      << ",\"authority\":\"diagnostic-service-cost-only\""
      << ",\"clock\":\"std::chrono::steady_clock\",\"units\":\"nanoseconds\""
      << ",\"thread_cpu_time\":false,\"additive\":false,\"observed\":" << boolean(observed)
      << ",\"outer_plan_admission_scope\":\"fresh-v4-execution-initialization-and-initial-setup-"
         "accounting-from-immutable-preparation;outside-prepared-replay-wall\""
      << ",\"replay_admission_scope\":\"replay-general-plan-v4-execution-admit;"
         "within-prepared-replay-wall\""
      << ",\"pre_arm_checkpoint_scope\":\"begin-render-tape-checkpoint-pre-arm;"
         "within-prepared-replay-wall\""
      << ",\"finish_scope\":\"v4-execution-finish-including-actual-qualification;"
         "within-prepared-replay-wall\"";
  printNanos("outer_plan_admission_ns", timing.v4PlanAdmissionNanos);
  printNanos("replay_admission_ns", timing.v4ReplayAdmissionNanos);
  printNanos("pre_arm_checkpoint_ns", timing.v4PreArmCheckpointNanos);
  printNanos("finish_ns", timing.v4FinishNanos);
  out << '}';
}

void printProviderCallTimingMeasurements(std::ostream& out, const Evidence& evidence) {
  const bool observed = evidence.providerCallReturned && !evidence.options.inspect &&
      evidence.failure.empty() && evidence.qualified && evidence.actualMetal &&
      evidence.deviceCreated && evidence.teardownComplete && evidence.executionCount == 1u;
  out << "{\"schema\":\"dxmt9.render_tape.v4_provider_call_timing.v2\""
      << ",\"authority\":\"diagnostic-service-cost-only\""
      << ",\"clock\":\"std::chrono::steady_clock\",\"units\":\"nanoseconds\""
      << ",\"thread_cpu_time\":false,\"additive\":false,\"observed\":" << boolean(observed)
      << ",\"provider_call_scope\":\"caller-full-expression-replayPreparedRenderTapeIdentityV4-"
         "including-return-local-destruction-result-assignment-and-existing-result-replacement;"
         "excludes-input-preparation-execution-option-setup-factory-creation-device-teardown\""
      << ",\"included_phases\":\"fresh-v4-execution-initialization-initial-setup-accounting-"
         "and-executor-destruction\""
      << ",\"nesting_scope\":\"includes-prepared-replay-wall-and-v4-phases;"
         "not-additive-with-other-service-scopes\""
      << ",\"zero_semantics\":\"below-clock-resolution;not-proof-of-no-provider-work\""
      << ",\"null_semantics\":\"unobserved-or-unqualified\""
      << ",\"provider_call_wall_ns\":";
  if (observed) out << evidence.providerCallWallNanos; else out << "null";
  out << '}';
}

void printImmutableV4Preparation(std::ostream& out, const Evidence& evidence) {
  const auto& prepared = evidence.result.prepared;
  const auto& execution = evidence.result.exactDiagnosticV4.execution;
  const bool observed = !evidence.options.inspect && evidence.failure.empty() &&
      evidence.qualified && evidence.actualMetal && evidence.deviceCreated && evidence.teardownComplete &&
      evidence.executionCount == 1u && prepared.immutableV4PreparationOwned &&
      prepared.v4PreparationCount == 1u && prepared.reusedImmutablePreparation &&
      execution.admissionCount == 1u && execution.preparedAdmissionConsumed;
  const auto printNanos = [&](const char* name, std::uint64_t value) {
    out << ',' << quoted(name) << ':';
    if (observed) out << value; else out << "null";
  };
  const auto printFlag = [&](const char* name, bool value) {
    out << ',' << quoted(name) << ':';
    if (observed) out << boolean(value); else out << "null";
  };
  out << "{\"schema\":\"dxmt9.render_tape.v4_immutable_preparation.v1\""
      << ",\"authority\":\"diagnostic-immutable-input-validity-only\""
      << ",\"clock\":\"std::chrono::steady_clock\",\"units\":\"nanoseconds\""
      << ",\"thread_cpu_time\":false,\"additive\":false,\"observed\":" << boolean(observed)
      << ",\"preparation_scope\":\"one-off-provider-v4-source-plan-execution-template-and-initial-"
         "setup-plan-construction;excludes-owned-tape-blob-copy-general-preflight-independent-"
         "diagnostic-inspection-and-device-effects\""
      << ",\"sharing_scope\":\"one-off-provider-preparation-reading-repeated-per-execution;"
         "not-per-iteration-work-or-an-aggregate\""
      << ",\"zero_semantics\":\"below-clock-resolution;not-proof-of-no-preparation-work\""
      << ",\"null_semantics\":\"unobserved-or-unqualified\"";
  printFlag("immutable_preparation_owned", prepared.immutableV4PreparationOwned);
  printNanos("preparation_count", prepared.v4PreparationCount);
  printFlag("reused_for_execution", prepared.reusedImmutablePreparation);
  printNanos("fresh_execution_admission_count", execution.admissionCount);
  printFlag("prepared_handoff_consumed", execution.preparedAdmissionConsumed);
  printNanos("v4_prepare_wall_ns", prepared.timing.v4PreparationNanos);
  out << '}';
}

void printAdmissionExecution(std::ostream& out, const Evidence& evidence) {
  const auto& execution = evidence.result.exactDiagnosticV4.execution;
  const bool observed = !evidence.options.inspect && evidence.failure.empty() &&
      evidence.qualified && evidence.actualMetal && evidence.deviceCreated &&
      evidence.teardownComplete && evidence.executionCount == 1u;
  out << "{\"schema\":\"dxmt9.render_tape.v4_admission_execution.v1\""
      << ",\"observed\":" << boolean(observed) << ",\"admission_count\":";
  if (observed) out << execution.admissionCount; else out << "null";
  out << ",\"prepared_handoff_consumed\":";
  if (observed) out << boolean(execution.preparedAdmissionConsumed); else out << "null";
  out << '}';
}

void print(std::ostream& out, const Evidence& evidence) {
  const auto& result = evidence.result;
  const auto& v4 = result.exactDiagnosticV4.execution;
  const char* status = evidence.failure.empty() ? (evidence.options.inspect ? "inspection-complete" : "complete") : "rejected";
  const auto failureEvent = evidence.preflight.failedEventIndex != noIndex
      ? evidence.preflight.failedEventIndex : evidence.wire.failedEventIndex;
  out << "{\"schema\":\"dxmt9.render_tape.v4_capture_diagnostic.v1\",\"diagnostic\":true,\"policy_qualified\":false,\"timing_eligible\":false"
      << ",\"status\":" << quoted(status) << ",\"phase\":" << quoted(evidence.phase)
      << ",\"failure\":" << quoted(evidence.failure.empty() ? "none" : evidence.failure)
      << ",\"bundle\":" << quoted(evidence.options.bundle.string()) << ",\"manifest_validation\":\"external-trusted-loader\""
      << ",\"replay_route\":" << quoted(evidence.options.route) << ",\"execution_count\":" << evidence.executionCount
      << ",\"producer_contract\":" << quoted(evidence.options.producerContract)
      << ",\"publication_mode\":" << quoted(evidence.options.publicationMode)
      << ",\"capacity_policy\":{\"slot_headroom_bytes\":" << evidence.options.capacityPolicy.slotHeadroomBytes
      << ",\"aggregate_headroom_bytes\":" << evidence.options.capacityPolicy.aggregateHeadroomBytes << '}'
      << ",\"replay_policy\":{\"schema\":\"dxmt9.render_tape.v4_replay_policy.v1\",\"route\":"
      << quoted(evidence.options.inspect ? "inspection" : evidence.options.route)
      << ",\"draw_command_limit\":" << evidence.options.drawCommandLimit
      << ",\"slot_headroom_bytes\":" << evidence.options.capacityPolicy.slotHeadroomBytes
      << ",\"aggregate_headroom_bytes\":" << evidence.options.capacityPolicy.aggregateHeadroomBytes << '}'
      << ",\"device_created\":" << boolean(evidence.deviceCreated)
      << ",\"prepared\":{\"immutable_input_owned\":" << boolean(result.prepared.immutableInputOwned)
      << ",\"preflight_completed\":" << boolean(result.prepared.preflightCompleted)
      << ",\"plan_reused\":" << boolean(result.prepared.planReused)
      << ",\"prepare_count\":" << result.prepared.prepareCount << '}'
      << ",\"qualified\":" << boolean(evidence.qualified) << ",\"query_qualified\":" << boolean(evidence.query)
      << ",\"exact_source_diagnostic_complete\":" << boolean(v4.exactSourceDiagnosticComplete)
      << ",\"semantic_mapping_qualified\":" << boolean(evidence.semanticMappingQualified)
      << ",\"pixels_qualified\":" << boolean(evidence.pixels) << ",\"source_exact\":" << boolean(evidence.sourceExact)
      << ",\"output_exact\":" << boolean(evidence.outputExact) << ",\"plan_sha256\":" << quoted(hexBytes(evidence.planDigest))
      << ",\"expected_plan_sha256\":" << quoted(hexBytes(evidence.options.plan))
      << ",\"stages\":{\"inputs_owned\":" << boolean(evidence.inputsOwned) << ",\"pair_valid\":" << boolean(evidence.pairValid)
      << ",\"general_preflight_valid\":" << boolean(evidence.preflightValid)
      << ",\"v4_diagnostic_admitted\":" << boolean(evidence.admitted)
      << ",\"exact_diagnostic_admitted\":" << boolean(evidence.admitted &&
          evidence.options.publicationMode == "exact-source-diagnostic")
      << ",\"oracle_inputs_valid\":" << boolean(evidence.oracleInputsValid) << ",\"expected_plan_digest_matched\":" << boolean(evidence.digestMatched)
      << ",\"fresh_device_created\":" << boolean(evidence.deviceCreated) << ",\"actual_metal\":" << boolean(evidence.actualMetal)
      << ",\"fresh_device_teardown_complete\":" << boolean(evidence.teardownComplete) << "}"
      << ",\"input\":{\"events_sha256\":" << quoted(hexBytes(evidence.options.events))
      << ",\"identity_sha256\":" << quoted(hexBytes(evidence.options.identity))
      << ",\"execution_topology_sha256\":" << quoted(hexBytes(evidence.options.topology))
      << ",\"action_profile_sha256\":" << quoted(hexBytes(evidence.options.actionProfile))
      << ",\"source_sha256\":" << quoted(hexBytes(evidence.options.source)) << ",\"output_sha256\":" << quoted(hexBytes(evidence.options.output))
      << ",\"blob_bytes\":" << evidence.blobBytes << ",\"oracle_bytes_each\":" << evidence.expectedPixels
      << ",\"events\":" << evidence.planEvents << ",\"records\":" << evidence.planRecords
      << ",\"sources\":" << evidence.planSources << ",\"commands\":" << evidence.planCommands
      << ",\"coordinators\":" << evidence.planCoordinators << "}"
      << ",\"action_profile\":{\"authenticated\":" << boolean(evidence.actionProfileAuthenticated)
      << ",\"stable_exact\":" << boolean(evidence.actionProfileStableExact)
      << ",\"evidence\":" << evidence.actionWitness.evidence
      << ",\"construction_profile\":" << evidence.actionWitness.constructionProfile
      << ",\"arm_profile\":" << evidence.actionWitness.armProfile
      << ",\"freeze_profile\":" << evidence.actionWitness.freezeProfile
      << ",\"capture_token\":" << evidence.actionWitness.captureToken
      << ",\"capture_epoch\":" << evidence.actionWitness.captureEpoch
      << ",\"queue\":" << evidence.actionWitness.queue
      << ",\"arm_revision\":" << evidence.actionWitness.armRevision
      << ",\"freeze_revision\":" << evidence.actionWitness.freezeRevision << "}"
      << ",\"query_type_counts\":{\"event\":" << evidence.queryTypes[0]
      << ",\"occlusion\":" << evidence.queryTypes[1] << ",\"timestamp\":" << evidence.queryTypes[2]
      << ",\"disjoint\":" << evidence.queryTypes[3] << ",\"frequency\":" << evidence.queryTypes[4] << "}"
      << ",\"preflight\":{\"wire_status\":" << quoted(renderTapeValidationStatusName(evidence.wire.status))
      << ",\"chunk_status_code\":" << static_cast<unsigned>(evidence.wire.chunkStatus)
      << ",\"provider_status\":" << quoted(frameTapeReplayStatusName(evidence.preflight.status)) << ",\"failed_event\":";
  if (failureEvent == noIndex) out << "null"; else out << failureEvent;
  out << ",\"failed_record\":";
  if (evidence.failedRecord == noIndex) out << "null"; else out << evidence.failedRecord;
  out << ",\"plan_status\":" << quoted(captureSourceReplayPlanV4StatusName(evidence.planStatus)) << "}"
      << ",\"provider_status\":" << quoted(frameTapeReplayStatusName(result.status)) << ",\"failed_event\":";
  if (result.failedEventIndex == noIndex) out << "null"; else out << result.failedEventIndex;
  out << ",\"v4\":{\"failure\":" << quoted(result.exactDiagnosticV4.failure)
      << ",\"validated\":" << boolean(v4.validated) << ",\"armed\":" << boolean(v4.armed) << ",\"complete\":" << boolean(v4.complete)
      << ",\"canonical_traversal_complete\":" << boolean(v4.canonicalTraversalComplete) << ",\"raw_ack_coverage_complete\":" << boolean(v4.rawAckCoverageComplete)
      << ",\"actual_attempts_observed\":" << boolean(v4.actualAttemptsObserved) << ",\"actual_lifecycle_complete\":" << boolean(v4.actualLifecycleComplete)
      << ",\"actual_topology_complete\":" << boolean(v4.actualTopologyComplete) << ",\"exact_source_diagnostic_complete\":" << boolean(v4.exactSourceDiagnosticComplete)
      << ",\"semantic_mapping_complete\":" << boolean(v4.semanticMappingComplete)
      << ",\"replayed_events\":" << v4.replayedEvents << ",\"dispatched_records\":" << v4.dispatchedRecords << ",\"acknowledged_raws\":" << v4.acknowledgedRaws
      << ",\"actual_attempts\":" << v4.actualIdentity.attempts.size() << ",\"actual_sources\":" << v4.actualIdentity.sources.size()
      << ",\"actual_initializers\":" << v4.actualIdentity.initializers.size() << ",\"actual_commands\":" << v4.actualIdentity.commands.size()
      << ",\"actual_coordinators\":" << v4.actualIdentity.coordinators.size() << ",\"actual_plans\":" << v4.actualTopology.plans.size()
      << ",\"actual_placements\":" << v4.actualTopology.placements.size() << ",\"mapped_sources\":" << v4.sources.size()
      << ",\"mapped_initializers\":" << v4.initializers.size() << ",\"mapped_coordinators\":" << v4.coordinators.size()
      << ",\"semantic_mapping\":{\"owned\":" << boolean(static_cast<bool>(v4.semanticMapping))
      << ",\"status\":" << quoted(renderTapeSemanticMappingV4StatusName(v4.semanticMappingStatus))
      << ",\"qualified\":" << boolean(evidence.semanticMappingQualified)
      << ",\"records\":" << (v4.semanticMapping ? v4.semanticMapping->records().size() : 0u)
      << ",\"command_relations\":" << (v4.semanticMapping ? v4.semanticMapping->commands().size() : 0u)
      << ",\"source_relations\":" << (v4.semanticMapping ? v4.semanticMapping->sources().size() : 0u)
      << ",\"raws\":" << (v4.semanticMapping ? v4.semanticMapping->raws().size() : 0u)
      << ",\"coordinators\":" << (v4.semanticMapping ? v4.semanticMapping->coordinators().size() : 0u)
      << ",\"obligations\":" << (v4.semanticMapping ? v4.semanticMapping->obligations().size() : 0u)
      << ",\"obligations_expected\":" << v4.sourceObligationsExpected
      << ",\"obligations_registered\":" << v4.sourceObligationsRegistered
      << ",\"obligations_pending\":" << v4.sourceObligationsPending
      << ",\"obligations_qualified\":" << v4.sourceObligationsQualified << "}"
      << ",\"grouping\":{\"captured_sources\":" << evidence.planSources
      << ",\"candidate_sources\":" << v4.actualIdentity.sources.size()
      << ",\"captured_commands\":" << evidence.planCommands
      << ",\"candidate_commands\":" << v4.actualIdentity.commands.size()
      << ",\"source_count_changed\":" << boolean(v4.actualLifecycleComplete &&
          evidence.planSources != v4.actualIdentity.sources.size())
      << ",\"command_grouping_changed\":" << boolean(evidence.commandGroupingChanged) << "}"
      << ",\"query_baseline_expected\":" << v4.queryBaselineExpected
      << ",\"query_baselines_joined\":" << v4.queryBaselinesJoined
      << ",\"query_objects_mapped\":" << v4.queryObjectsMapped
      << ",\"present_preparation\":{\"attempted\":" << boolean(v4.presentPreparationAttempted)
      << ",\"completed\":" << boolean(v4.presentPreparationCompleted)
      << ",\"hresult\":" << v4.presentPreparationHresult
      << ",\"event_index\":" << v4.presentPreparationEventIndex << "}"
      << ",\"initial_setup\":{\"admitted\":" << boolean(result.exactDiagnosticV4.initialSetupAdmitted)
      << ",\"completed\":" << boolean(result.exactDiagnosticV4.initialSetupCompleted)
      << ",\"bytes\":" << result.exactDiagnosticV4.initialSetupBytes
      << ",\"owners\":" << result.exactDiagnosticV4.initialSetupOwners
      << ",\"seeds\":" << result.exactDiagnosticV4.initialSetupSeeds
      << ",\"prepared\":" << result.exactDiagnosticV4.initialSetupPrepared
      << ",\"activated\":" << result.exactDiagnosticV4.initialSetupActivated
      << ",\"consumed\":" << result.exactDiagnosticV4.initialSetupConsumed
      << ",\"allocation_only_owners\":" << result.exactDiagnosticV4.initialSetupAllocationOnlyOwners
      << ",\"allocation_bytes\":" << result.exactDiagnosticV4.initialSetupAllocationBytes << "}"
      << ",\"mutation_drains\":{\"nooverwrite_without_extra_drain\":"
      << result.exactDiagnosticV4.noOverwriteWritesWithoutExtraDrain
      << ",\"interval_with_extra_drain\":" << result.exactDiagnosticV4.intervalMutationsWithExtraDrain << "}"
      << ",\"local_arm_attempted\":" << boolean(v4.localArmAttempted) << ",\"local_arm_hresult\":";
  if (v4.localArmAttempted) out << v4.localArmHresult; else out << "null";
  out << ",\"local_arm_status\":" << v4.localArm.status << ",\"local_arm_version\":" << v4.localArm.version
      << ",\"local_arm_token\":" << v4.localArm.captureToken << ",\"local_arm_epoch\":" << v4.localArm.captureEpoch
      << ",\"local_queue\":" << v4.localArm.queue << ",\"pre_arm\":";
  if (!v4.preArm) out << "null";
  else {
    const auto& inspected = *v4.preArm;
    const auto& state = inspected.checkpoint;
    out << "{\"next_sequence\":" << state.nextSequence
        << ",\"published_sequence\":" << state.publishedSequence << ",\"completed_sequence\":" << state.completedSequence
        << ",\"inflight_count\":" << state.inflightCount << ",\"build_quiescent\":" << boolean(state.buildQuiescent)
        << ",\"writing_empty\":" << boolean(state.writingEmpty) << ",\"terminal\":" << boolean(state.terminal)
        << ",\"initializer_present\":" << boolean(inspected.initializerPresent)
        << ",\"initializer_pending\":" << boolean(state.initializerPending)
        << ",\"initializer_tracking_available\":" << boolean(state.initializerTrackingAvailable)
        << ",\"initializer_progress_available\":" << boolean(inspected.initializerProgressAvailable)
        << ",\"initializer_published\":";
    if (inspected.initializerProgressAvailable) out << state.initializerPublished; else out << "null";
    out << ",\"initializer_completed\":";
    if (inspected.initializerProgressAvailable) out << state.initializerCompleted; else out << "null";
    out << ",\"initializer_failed\":";
    if (inspected.initializerProgressAvailable) out << boolean(state.initializerFailed); else out << "null";
    out << "}";
  }
  out << ",\"publication_cuts\":";
  printPublicationCuts(out, evidence);
  out << ",\"local_finish_status\":" << v4.localFinish.status << ",\"local_finish_bytes\":" << v4.localFinish.byteCount << "}"
      << ",\"pixels\":{\"source_readback\":" << boolean(result.validity.sourceReadback) << ",\"output_readback\":" << boolean(result.validity.outputReadback)
      << ",\"expected_digest_captured\":" << boolean(result.validity.expectedDigestCaptured)
      << ",\"expected_digest_matched\":" << boolean(result.validity.expectedDigestMatched)
      << ",\"output_non_degenerate\":" << boolean(result.validity.outputNonDegenerate)
      << ",\"source_bytes\":" << result.sourcePixels.size() << ",\"output_bytes\":" << result.outputPixels.size()
      << ",\"source_sha256\":" << quoted(hexBytes(result.validity.sourceDigest))
      << ",\"output_sha256\":" << quoted(hexBytes(result.validity.outputDigest)) << "}"
      << ",\"conservation\":{\"input_blobs\":" << result.conservation.inputBlobs << ",\"referenced_blobs\":" << result.conservation.referencedBlobs
      << ",\"objects_created\":" << result.conservation.objectsCreated << ",\"objects_released\":" << result.conservation.objectsReleased
      << ",\"present_ordinal\":" << result.conservation.presentOrdinal << ",\"completion_ordinal\":" << result.conservation.completionOrdinal << "}"
      << ",\"path_counters\":{\"observed\":" << boolean(evidence.countersObserved) << ",\"monotonic\":" << boolean(evidence.countersMonotonic);
  if (evidence.countersObserved && evidence.countersMonotonic)
    out << ",\"direct_leases\":" << evidence.after.directLeases - evidence.before.directLeases
        << ",\"direct_lease_draws\":" << evidence.after.directLeaseDraws - evidence.before.directLeaseDraws
        << ",\"compatibility_draws\":" << evidence.after.compatibilityDraws - evidence.before.compatibilityDraws
        << ",\"compatibility_batches\":" << evidence.after.compatibilityBatches - evidence.before.compatibilityBatches
        << ",\"compatibility_unbatched_draws\":" << evidence.after.compatibilityUnbatchedDraws - evidence.before.compatibilityUnbatchedDraws
        << ",\"encode_chunks\":" << evidence.after.encodeChunks - evidence.before.encodeChunks
        << ",\"gpu_errors\":" << evidence.after.gpuErrors - evidence.before.gpuErrors;
  out << "},\"timing_measurements\":";
  printTimingMeasurements(out, evidence);
  out << ",\"query_timing_measurements\":";
  printQueryTimingMeasurements(out, evidence);
  out << ",\"v4_phase_timing_measurements\":";
  printV4PhaseTimingMeasurements(out, evidence);
  out << ",\"provider_call_timing_measurements\":";
  printProviderCallTimingMeasurements(out, evidence);
  out << ",\"immutable_v4_preparation\":";
  printImmutableV4Preparation(out, evidence);
  out << ",\"admission_execution\":";
  printAdmissionExecution(out, evidence);
  out << ",\"query_expected\":";
  printQuery(out, evidence.preflight.queryReplay);
  out << ",\"query_actual\":";
  printQuery(out, result.queryReplay);
  out << ",\"readback_artifacts\":";
  printReadbackArtifacts(out, evidence);
  out << ",\"encode_draw_witness\":";
  printEncodeDrawWitness(out, evidence);
  out << ",\"loaded_images\":[";
  for (std::size_t index = 0u; index < evidence.loadedImages.size(); ++index) {
    const auto& image = evidence.loadedImages[index];
    if (index) out << ',';
    out << "{\"path\":" << quoted(image.path) << ",\"bytes\":" << image.bytes
        << ",\"sha256\":" << quoted(hexBytes(image.digest)) << '}';
  }
  out << "],\"effective_stage_environment\":{";
  bool firstEnvironment = true;
  for (const auto& [name, value] : evidence.effectiveStageEnvironment) {
    if (!firstEnvironment) out << ',';
    firstEnvironment = false;
    out << quoted(name) << ':' << quoted(value);
  }
  out << '}';
  if (evidence.iterationOrdinal != noIndex)
    out << ",\"iteration\":{\"ordinal\":" << evidence.iterationOrdinal
        << ",\"kind\":" << quoted(evidence.iterationKind)
        << ",\"kind_index\":" << evidence.iterationKindIndex << '}';
  out << "}\n";
}

void reauthenticateLoadedImages(Evidence& evidence) {
  try {
    const auto images = collectLoadedImages();
    require(evidence.loadedImages.empty() || images == evidence.loadedImages,
        "loaded-image-fingerprint-changed-during-diagnostic");
    evidence.loadedImages = images;
  } catch (const std::exception& error) {
    evidence.loadedImages.clear();
    evidence.qualified = false;
    if (evidence.failure.empty()) {
      evidence.phase = "loaded-image-reauthentication";
      evidence.failure = error.what();
    }
  } catch (...) {
    evidence.loadedImages.clear();
    evidence.qualified = false;
    if (evidence.failure.empty()) {
      evidence.phase = "loaded-image-reauthentication";
      evidence.failure = "loaded-image-reauthentication-exception";
    }
  }
}

// Bound serialization itself, including failed receipts. Never grow an
// unrestricted string and discover the receipt limit after construction.
class BoundedJsonBuffer final : public std::streambuf {
public:
  explicit BoundedJsonBuffer(std::size_t limit) : limit_(limit) {}
  const std::string& bytes() const noexcept { return bytes_; }
  bool exceeded() const noexcept { return exceeded_; }
protected:
  std::streamsize xsputn(const char* bytes, std::streamsize count) override {
    if (count <= 0) return 0;
    const auto available = limit_ - bytes_.size();
    const auto amount = std::min<std::uint64_t>(available, static_cast<std::uint64_t>(count));
    bytes_.append(bytes, static_cast<std::size_t>(amount));
    if (amount != static_cast<std::uint64_t>(count)) exceeded_ = true;
    return static_cast<std::streamsize>(amount);
  }
  int_type overflow(int_type value) override {
    if (traits_type::eq_int_type(value, traits_type::eof())) return traits_type::not_eof(value);
    if (bytes_.size() == limit_) {
      exceeded_ = true;
      return traits_type::eof();
    }
    bytes_.push_back(traits_type::to_char_type(value));
    return value;
  }
private:
  std::string bytes_{};
  std::size_t limit_ = 0u;
  bool exceeded_ = false;
};

struct IterationManifest {
  std::uint32_t ordinal = 0u, kindIndex = 0u;
  const char* kind = "none";
  std::string directory{};
  std::uint64_t resultBytes = 0u;
  RenderTapeDigest resultDigest{};
  bool complete = false;
};

struct RepetitionEvidence {
  std::uint64_t prepareCount = 0u, jsonBytes = 0u;
  std::vector<IterationManifest> iterations{};
};

std::string iterationDirectory(const char* kind, std::size_t index) {
  const auto number = std::to_string(index);
  require(number.size() <= 4u, "iteration-index-out-of-bounds");
  return std::string("iterations/") + kind + '-' + std::string(4u - number.size(), '0') + number;
}

Evidence iterationEvidence(const Evidence& common) {
  Evidence row;
  row.options = common.options;
  row.inputsOwned = common.inputsOwned;
  row.pairValid = common.pairValid;
  row.preflightValid = common.preflightValid;
  row.admitted = common.admitted;
  row.oracleInputsValid = common.oracleInputsValid;
  row.digestMatched = common.digestMatched;
  row.actionProfileAuthenticated = common.actionProfileAuthenticated;
  row.actionProfileStableExact = common.actionProfileStableExact;
  row.loadedImages = common.loadedImages;
  row.actionWitness = common.actionWitness;
  row.blobBytes = common.blobBytes;
  row.expectedPixels = common.expectedPixels;
  row.planDigest = common.planDigest;
  row.wire = common.wire;
  row.preflight = common.preflight;
  row.admission = common.admission;
  row.planStatus = common.planStatus;
  row.planEvents = common.planEvents;
  row.planRecords = common.planRecords;
  row.planSources = common.planSources;
  row.planCommands = common.planCommands;
  row.planCoordinators = common.planCoordinators;
  row.planInitializers = common.planInitializers;
  row.queryTypes = common.queryTypes;
  row.expectedSources = common.expectedSources;
  row.expectedInitializers = common.expectedInitializers;
  row.reportedSources = common.reportedSources;
  row.reportedInitializers = common.reportedInitializers;
  return row;
}

void executeRepeated(Evidence& evidence, RepetitionEvidence& repetitions) {
  DiagnosticPreparedInput session;
  prepareDiagnostic(evidence, session);
  repetitions.prepareCount = session.prepared.prepareCount();
  const auto total = evidence.options.warmup + evidence.options.repeat;
  evidence.phase = "repetition-budget-admission";
  require(evidence.expectedPixels <= repeatedReadbackByteLimit / 2u / total,
      "repetition-readback-byte-limit");
  repetitions.iterations.reserve(total);
  std::vector<std::unique_ptr<ArtifactFileDescriptor>> directories;
  directories.reserve(total);
  // All destinations are launcher-precreated private directories. Pin and
  // reject existing artifacts before the first actual device effect.
  evidence.phase = "repetition-artifact-admission";
  for (std::size_t ordinal = 0u; ordinal < total; ++ordinal) {
    const bool warmup = ordinal < evidence.options.warmup;
    const auto index = warmup ? ordinal : ordinal - evidence.options.warmup;
    const auto relative = iterationDirectory(warmup ? "warmup" : "measured", index);
    auto directory = std::make_unique<ArtifactFileDescriptor>();
    const auto* failure = openReadbackDirectory(evidence.options.readbackDirectory / relative, *directory);
    require(std::strcmp(failure, "none") == 0, failure);
    for (const auto* name : {"native.json", "replay-source.rgba", "replay-output.rgba"}) {
      struct stat status{};
      require(::fstatat(directory->get(), name, &status, AT_SYMLINK_NOFOLLOW) != 0 &&
          errno == ENOENT, "repetition-artifact-already-exists-or-unavailable");
    }
    directories.push_back(std::move(directory));
  }
  for (std::size_t ordinal = 0u; ordinal < total; ++ordinal) {
    require(repetitions.jsonBytes < repeatedJsonByteLimit, "repetition-json-byte-limit");
    const bool warmup = ordinal < evidence.options.warmup;
    const auto index = warmup ? ordinal : ordinal - evidence.options.warmup;
    const auto* kind = warmup ? "warmup" : "measured";
    const auto relative = iterationDirectory(kind, index);
    // At most one mutable result owner exists. The immutable session above
    // outlives every borrowed plan span and all per-iteration GPU teardown.
    auto row = iterationEvidence(evidence);
    row.options.readbackDirectory /= relative;
    row.iterationOrdinal = static_cast<std::uint32_t>(ordinal);
    row.iterationKind = kind;
    row.iterationKindIndex = static_cast<std::uint32_t>(index);
    try {
      executeIteration(row, session);
    } catch (const std::exception& error) {
      row.failure = error.what();
      row.qualified = false;
    } catch (...) {
      row.failure = "unknown-iteration-exception";
      row.qualified = false;
    }
    reauthenticateLoadedImages(row);
    // Failed results cannot escape the pre-admitted raw-byte budget.
    if (row.result.sourcePixels.size() > evidence.expectedPixels ||
        row.result.outputPixels.size() > evidence.expectedPixels) {
      row.readbacks.requested = true;
      row.readbacks.failure = "repetition-readback-size-out-of-bounds";
      describeReadback(row.result.sourcePixels, row.readbacks.source);
      describeReadback(row.result.outputPixels, row.readbacks.output);
      row.readbacks.source.failure = row.readbacks.output.failure = row.readbacks.failure;
      row.qualified = false;
      if (row.failure.empty()) row.failure = row.readbacks.failure;
    } else preserveReadbacks(row);
    evidence.executionCount += row.executionCount;
    evidence.effectiveStageEnvironment = row.effectiveStageEnvironment;
    const auto limit = std::min(iterationJsonByteLimit,
        repeatedJsonByteLimit - repetitions.jsonBytes);
    BoundedJsonBuffer buffer(static_cast<std::size_t>(limit));
    std::ostream output(&buffer);
    try {
      print(output, row);
    } catch (...) {
      row.qualified = false;
      if (row.failure.empty()) row.failure = "iteration-json-serialization-failed";
    }
    if (!output.good() || buffer.exceeded()) {
      row.qualified = false;
      if (row.failure.empty()) row.failure = limit < iterationJsonByteLimit
          ? "repetition-json-byte-limit" : "iteration-json-byte-limit";
    }
    const auto& json = buffer.bytes();
    const std::span<const std::byte> bytes{
        reinterpret_cast<const std::byte*>(json.data()), json.size()};
    ReadbackArtifact artifact;
    describeReadback(bytes, artifact);
    writeReadback(directories[ordinal]->get(), "native.json", bytes, artifact);
    if (!artifact.written || !artifact.digestComputed) {
      evidence.qualified = false;
      evidence.phase = "iteration-json-artifact";
      // A secondary artifact failure cannot conceal an original rendering,
      // query, image-authentication, or serialization failure.
      evidence.failure = row.failure.empty() ? "iteration-json-artifact-write-failed" : row.failure;
      return;
    }
    repetitions.jsonBytes += artifact.writtenBytes;
    repetitions.iterations.push_back({
        .ordinal = static_cast<std::uint32_t>(ordinal),
        .kindIndex = static_cast<std::uint32_t>(index), .kind = kind, .directory = relative,
        .resultBytes = artifact.writtenBytes, .resultDigest = artifact.digest,
        .complete = row.failure.empty() && row.qualified});
    if (!row.failure.empty() || !row.qualified) {
      evidence.qualified = false;
      evidence.phase = "iteration-rejected";
      evidence.failure = row.failure.empty() ? "iteration-qualification-rejected" : row.failure;
      return;
    }
  }
  evidence.qualified = repetitions.prepareCount == 1u && repetitions.iterations.size() == total &&
      evidence.executionCount == total && std::all_of(repetitions.iterations.begin(),
          repetitions.iterations.end(), [](const auto& row) { return row.complete; });
  evidence.phase = evidence.qualified ? "repetitions-complete" : "repetitions-rejected";
  if (!evidence.qualified) evidence.failure = "repetition-qualification-rejected";
}

void printRepeated(std::ostream& out, const Evidence& evidence, const RepetitionEvidence& repetitions) {
  const bool complete = evidence.failure.empty() && evidence.qualified;
  out << "{\"schema\":\"dxmt9.render_tape.v4_capture_diagnostic_repeat.v1\""
      << ",\"diagnostic\":true,\"policy_qualified\":false,\"timing_eligible\":false"
      << ",\"qualified\":" << boolean(complete)
      << ",\"status\":" << quoted(complete ? "complete" : "rejected")
      << ",\"failure\":" << quoted(evidence.failure.empty() ? "none" : evidence.failure)
      << ",\"phase\":" << quoted(evidence.phase)
      << ",\"producer_contract\":" << quoted(evidence.options.producerContract)
      << ",\"publication_mode\":" << quoted(evidence.options.publicationMode)
      << ",\"plan_sha256\":" << quoted(hexBytes(evidence.planDigest))
      << ",\"expected_plan_sha256\":" << quoted(hexBytes(evidence.options.plan))
      << ",\"replay_route\":" << quoted(evidence.options.route)
      << ",\"execution_count\":" << evidence.executionCount
      << ",\"prepare_count\":" << repetitions.prepareCount
      << ",\"repetition_policy\":{\"warmup\":" << evidence.options.warmup
      << ",\"repeat\":" << evidence.options.repeat
      << ",\"reset_policy\":\"fresh-device\",\"prepared_input_shared\":true"
      << ",\"cache_qualification\":\"uncontrolled-process-and-driver-caches\""
      << ",\"shared_preparation\":\"once;per-iteration-prepare-wall-ns-is-the-same-one-off-reading\""
      << ",\"input_storage\":\"prepared-owner-plus-retained-loader-copy\"}"
      << ",\"capacity_policy\":{\"slot_headroom_bytes\":" << evidence.options.capacityPolicy.slotHeadroomBytes
      << ",\"aggregate_headroom_bytes\":" << evidence.options.capacityPolicy.aggregateHeadroomBytes << '}'
      << ",\"replay_policy\":{\"schema\":\"dxmt9.render_tape.v4_replay_policy.v1\",\"route\":"
      << quoted(evidence.options.route) << ",\"draw_command_limit\":" << evidence.options.drawCommandLimit
      << ",\"slot_headroom_bytes\":" << evidence.options.capacityPolicy.slotHeadroomBytes
      << ",\"aggregate_headroom_bytes\":" << evidence.options.capacityPolicy.aggregateHeadroomBytes << '}'
      << ",\"iterations\":[";
  for (std::size_t index = 0u; index < repetitions.iterations.size(); ++index) {
    const auto& row = repetitions.iterations[index];
    if (index) out << ',';
    out << "{\"ordinal\":" << row.ordinal << ",\"kind\":" << quoted(row.kind)
        << ",\"kind_index\":" << row.kindIndex
        << ",\"result_path\":" << quoted(row.directory + "/native.json")
        << ",\"result_bytes\":" << row.resultBytes
        << ",\"result_sha256\":" << quoted(hexBytes(row.resultDigest))
        << ",\"readback_directory\":" << quoted(row.directory)
        << ",\"status\":" << quoted(row.complete ? "complete" : "rejected") << '}';
  }
  out << "],\"loaded_images\":[";
  for (std::size_t index = 0u; index < evidence.loadedImages.size(); ++index) {
    const auto& image = evidence.loadedImages[index];
    if (index) out << ',';
    out << "{\"path\":" << quoted(image.path) << ",\"bytes\":" << image.bytes
        << ",\"sha256\":" << quoted(hexBytes(image.digest)) << '}';
  }
  out << "],\"effective_stage_environment\":{";
  bool first = true;
  for (const auto& [name, value] : evidence.effectiveStageEnvironment) {
    if (!first) out << ',';
    first = false;
    out << quoted(name) << ':' << quoted(value);
  }
  out << "}}\n";
}

} // namespace

int main(int argc, char** argv) {
  Evidence evidence;
  RepetitionEvidence repetitions;
  try {
    evidence.options = arguments(argc, argv);
    evidence.phase = "loaded-image-authentication";
    evidence.loadedImages = collectLoadedImages();
    if (evidence.options.repeated) executeRepeated(evidence, repetitions);
    else execute(evidence);
  } catch (const std::exception& error) {
    evidence.failure = error.what();
  } catch (...) {
    evidence.failure = "unknown-exception";
  }
  // Both successful inspection and replay bind actual runtime files. Device
  // owners have already torn down here; no hashing occurs in partial JSON.
  reauthenticateLoadedImages(evidence);
  // execute's local device/factory owners have been released or unwound.
  // Only the retained actual result vectors are eligible for preservation.
  if (evidence.options.repeated) printRepeated(std::cout, evidence, repetitions);
  else {
    preserveReadbacks(evidence);
    print(std::cout, evidence);
  }
  return evidence.failure.empty() ? 0 : 1;
}
