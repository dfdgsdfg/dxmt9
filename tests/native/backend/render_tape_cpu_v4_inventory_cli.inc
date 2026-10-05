// Included after the routing fixture and its file/digest helpers, inside the
// existing anonymous namespace. This mode owns one cold CPU session only.
// The trusted launcher separately validates PE producer attestation; the
// native helper authenticates bytes, not manifest/environment authority.

namespace controlled_inventory_cli {

namespace replay = dxmt9::d3d9;
namespace runtime = dxmt9::d3d9::render_tape_runtime;
namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;
using Executor = dxmt9::test::CpuGeneralTapeExecutor<
    RuntimeFixture, dxmt9::CommandQueueArenaLeaseTestAccess>;
using Result = dxmt9::test::CpuGeneralTapeControlledV4InventoryResult;

constexpr std::size_t componentLimit = 64u * 1024u * 1024u;
constexpr std::uint64_t maxWallMilliseconds = 300000u;

struct Options {
  bool logicalSemantics = false;
  bool executionOnly = false;
  bool legalCandidateNoOverwriteContract = false;
  replay::ProviderReplayPolicy route = replay::ProviderReplayPolicy::Compatibility;
  std::size_t drawCommandLimit = 0u;
  dxmt9::test::RenderTapeV4CapacityPolicy capacityPolicy{};
  fs::path bundle{};
  replay::RenderTapeDigest events{}, identity{}, topology{}, source{}, output{}, action{}, plan{};
  std::string producerContract{};
  std::uint64_t wallMilliseconds = 0u;
};

struct Evidence {
  Options options{};
  std::string phase = "arguments", failure{}, inputComponent{};
  bool inputsAuthenticated = false, wireValid = false, generalValid = false, pairValid = false;
  bool actionAuthenticated = false, stableExact = false, oracleBytesAuthenticated = false;
  bool planDigestMatched = false, resourceDeviceCreated = false, fixtureCreated = false;
  bool cpuReadyArenaSupported = false, directSlotSupported = false;
  bool workersDisabled = false, offloadAbsent = false, localExactProfile = false;
  std::uint32_t localConstructionProfile = 0u, localCurrentProfile = 0u;
  std::uint64_t localProfileRevision = 0u;
  bool teardownComplete = false, authorityScopeValid = false;
  std::uint64_t blobBytes = 0u;
  std::size_t blobCount = 0u, planEvents = 0u, planRecords = 0u, planSources = 0u;
  std::uint32_t failedEvent = 0xffffffffu;
  std::uint64_t failedEventOrdinal = 0u;
  replay::RenderTapeDigest planDigest{};
  replay::RenderTapeActionWitnessV1 actionWitness{};
  replay::RenderTapeValidationResult wire{};
  replay::FrameTapeReplayResult preflight{};
  std::optional<dxmt9::test::ControlledCpuV4InputAdmission> inputAdmission{};
  std::optional<Result> result{};
};

void require(bool condition, const char* reason) {
  if (!condition) throw std::runtime_error(reason);
}

const char* boolean(bool value) noexcept { return value ? "true" : "false"; }

const char* routeName(replay::ProviderReplayPolicy route) noexcept {
  switch (route) {
  case replay::ProviderReplayPolicy::Compatibility: return "compatibility";
  case replay::ProviderReplayPolicy::ProductionSlot: return "production-slot";
  default: return "unsupported";
  }
}

std::string quoted(std::string_view value) {
  constexpr char hex[] = "0123456789abcdef";
  std::string text = "\"";
  for (const unsigned char byte : value) {
    if (byte == '"' || byte == '\\') {
      text.push_back('\\');
      text.push_back(static_cast<char>(byte));
    } else if (byte < 0x20u) {
      text += "\\u00";
      text.push_back(hex[byte >> 4u]);
      text.push_back(hex[byte & 15u]);
    } else text.push_back(static_cast<char>(byte));
  }
  return text + '"';
}

// std::quoted is visible in the including TU; keep string arguments on this
// JSON-control-character-aware overload instead of its ADL-selected template.
std::string quoted(const std::string& value) {
  return quoted(std::string_view(value));
}

replay::RenderTapeDigest digest(std::string_view value) {
  require(isNonzeroLowerSha256Hex(value), "digest-must-be-nonzero-lowercase-sha256");
  const auto nibble = [](char c) -> unsigned {
    return c <= '9' ? static_cast<unsigned>(c - '0')
                    : static_cast<unsigned>(c - 'a') + 10u;
  };
  replay::RenderTapeDigest out{};
  for (std::size_t i = 0u; i < out.size(); ++i)
    out[i] = static_cast<std::byte>((nibble(value[i * 2u]) << 4u) | nibble(value[i * 2u + 1u]));
  return out;
}

std::uint64_t wallBudget(std::string_view value) {
  require(!value.empty(), "max-wall-ms-must-be-positive-bounded-integer");
  std::uint64_t out = 0u;
  for (const auto c : value) {
    require(c >= '0' && c <= '9', "max-wall-ms-must-be-positive-bounded-integer");
    const auto digit = static_cast<unsigned>(c - '0');
    require(out <= (maxWallMilliseconds - digit) / 10u, "max-wall-ms-out-of-bounds");
    out = out * 10u + digit;
  }
  require(out && out <= maxWallMilliseconds, "max-wall-ms-out-of-bounds");
  return out;
}

Options arguments(int argc, char** argv) {
  require(argc >= 2 && (std::string_view(argv[1]) == "--controlled-cpu-v4-inventory" ||
      std::string_view(argv[1]) == "--controlled-cpu-v4-semantics" ||
      std::string_view(argv[1]) == "--controlled-cpu-v4-execute"),
      "dedicated-controlled-cpu-v4-mode-required");
  Options options;
  options.logicalSemantics = std::string_view(argv[1]) == "--controlled-cpu-v4-semantics";
  options.executionOnly = std::string_view(argv[1]) == "--controlled-cpu-v4-execute";
  std::vector<std::string_view> seen;
  for (int i = 2; i < argc; ++i) {
    const std::string_view option = argv[i];
    require(std::find(seen.begin(), seen.end(), option) == seen.end(), "duplicate-option");
    seen.push_back(option);
    require(i + 1 < argc, "missing-option-value");
    const std::string_view value = argv[++i];
    if (option == "--bundle") options.bundle = std::string(value);
    else if (option == "--events-sha256") options.events = digest(value);
    else if (option == "--identity-sha256") options.identity = digest(value);
    else if (option == "--execution-topology-sha256") options.topology = digest(value);
    else if (option == "--source-sha256") options.source = digest(value);
    else if (option == "--output-sha256") options.output = digest(value);
    else if (option == "--action-profile-sha256") options.action = digest(value);
    else if (option == "--expected-plan-digest") options.plan = digest(value);
    else if (option == "--max-wall-ms") options.wallMilliseconds = wallBudget(value);
    else if (option == "--producer-contract") options.producerContract = value;
    else if (option == "--route") {
      require(options.logicalSemantics || options.executionOnly, "route-option-requires-controlled-execution");
      if (value == "compatibility") options.route = replay::ProviderReplayPolicy::Compatibility;
      else if (value == "production-slot") options.route = replay::ProviderReplayPolicy::ProductionSlot;
      else throw std::runtime_error("unsupported-controlled-semantics-route");
    }
    else if (option == "--draw-command-limit") {
      require(options.logicalSemantics || options.executionOnly,
          "draw-limit-option-requires-controlled-execution");
      require(dxmt9::test::parseRenderTapeV4CapacityBytes(value, options.drawCommandLimit),
          "draw-command-limit-must-be-strict-decimal");
    }
    else if (option == "--slot-headroom-bytes" || option == "--aggregate-headroom-bytes") {
      require(options.logicalSemantics || options.executionOnly, "capacity-option-requires-controlled-execution");
      auto& bytes = option == "--slot-headroom-bytes" ? options.capacityPolicy.slotHeadroomBytes
                                                     : options.capacityPolicy.aggregateHeadroomBytes;
      require(dxmt9::test::parseRenderTapeV4CapacityBytes(value, bytes),
          "capacity-bytes-must-be-strict-decimal");
    }
    else if (option == "--buffer-preservation-contract") {
      require((options.logicalSemantics || options.executionOnly) &&
          value == "candidate-legal-nooverwrite-closed-writers-v1",
          "unsupported-buffer-preservation-contract");
      options.legalCandidateNoOverwriteContract = true;
    }
    else throw std::runtime_error("unknown-controlled-inventory-option");
  }
  for (const auto* required : {"--bundle", "--events-sha256", "--identity-sha256",
          "--execution-topology-sha256", "--source-sha256", "--output-sha256",
          "--action-profile-sha256", "--expected-plan-digest", "--max-wall-ms", "--producer-contract"})
    require(std::find(seen.begin(), seen.end(), required) != seen.end(), "required-option-missing");
  require(!options.bundle.empty(), "bundle-path-empty");
  require(options.producerContract == "none" ||
      options.producerContract == "pe-present-reservation-v1", "unsupported-producer-contract");
  require(!(options.logicalSemantics || options.executionOnly) || options.legalCandidateNoOverwriteContract,
      "explicit-candidate-buffer-preservation-contract-required");
  require(!(options.logicalSemantics || options.executionOnly) ||
      std::find(seen.begin(), seen.end(), "--route") != seen.end(),
      "explicit-controlled-semantics-route-required");
  require(dxmt9::test::renderTapeV4CapacityFlagsPaired(
      std::find(seen.begin(), seen.end(), "--slot-headroom-bytes") != seen.end(),
      std::find(seen.begin(), seen.end(), "--aggregate-headroom-bytes") != seen.end()),
      "capacity-options-must-be-paired");
  require(dxmt9::test::renderTapeV4ReplayPolicyValid({
      .productionSlot = options.route == replay::ProviderReplayPolicy::ProductionSlot,
      .drawCommandLimit = options.drawCommandLimit,
      .capacity = options.capacityPolicy}), "controlled-v4-replay-policy-rejected");
  return options;
}

void checkDeadline(Clock::time_point deadline) {
  require(Clock::now() < deadline, "controlled-inventory-input-deadline");
}

std::vector<std::byte> read(const fs::path& path, std::uint64_t limit,
                           Clock::time_point deadline) {
  checkDeadline(deadline);
  require(fs::symlink_status(path).type() == fs::file_type::regular, "input-not-regular-file");
  require(limit <= std::numeric_limits<std::size_t>::max(), "input-limit-not-addressable");
  std::vector<std::byte> bytes;
  require(readBoundedBinaryFile(path.string(), static_cast<std::size_t>(limit), bytes) &&
      !bytes.empty(), "bounded-input-read-failed");
  require(fs::file_size(path) == bytes.size(), "input-size-changed");
  checkDeadline(deadline);
  return bytes;
}

std::vector<std::byte> component(const Options& options, const char* name,
    const replay::RenderTapeDigest& expected, Clock::time_point deadline,
    std::uint64_t limit = componentLimit) {
  auto bytes = read(options.bundle / name, limit, deadline);
  require(replay::RenderTapeCaptureSession::sha256(bytes) == expected, "component-digest-mismatch");
  checkDeadline(deadline);
  return bytes;
}

struct Inputs {
  std::vector<std::byte> events{}, identity{}, topology{}, source{}, output{}, action{};
  std::vector<std::vector<std::byte>> storage{};
  std::vector<replay::RenderTapeProviderBlob> blobs{};
  replay::RenderTapeBlobCatalogue catalogue{};
};

Inputs load(Evidence& evidence, Clock::time_point deadline) {
  const auto& options = evidence.options;
  require(fs::symlink_status(options.bundle).type() == fs::file_type::directory,
      "bundle-not-directory");
  Inputs input;
  evidence.inputComponent = "events.bin";
  input.events = component(options, "events.bin", options.events, deadline);
  evidence.inputComponent = "identity.bin";
  input.identity = component(options, "identity.bin", options.identity, deadline);
  evidence.inputComponent = "execution_topology.bin";
  input.topology = component(options, "execution_topology.bin", options.topology, deadline);
  evidence.inputComponent = "source.rgba";
  input.source = component(options, "source.rgba", options.source, deadline);
  evidence.inputComponent = "output.rgba";
  input.output = component(options, "output.rgba", options.output, deadline);
  evidence.inputComponent = "action_profile.bin";
  input.action = component(options, "action_profile.bin", options.action, deadline,
      replay::kRenderTapeActionProfileV1Bytes);
  evidence.inputComponent = "blobs";
  const auto directory = options.bundle / "blobs";
  require(fs::symlink_status(directory).type() == fs::file_type::directory, "blobs-not-directory");
  std::vector<fs::path> paths;
  for (const auto& entry : fs::directory_iterator(directory)) {
    checkDeadline(deadline);
    require(paths.size() < replay::kRenderTapeHardMaxBlobEntries, "blob-entry-limit");
    const auto name = entry.path().filename().string();
    require(name.size() == 68u && name.ends_with(".bin"), "blob-name-not-canonical");
    (void)digest(std::string_view(name).substr(0u, 64u));
    require(entry.symlink_status().type() == fs::file_type::regular, "blob-not-regular-file");
    paths.push_back(entry.path());
  }
  std::sort(paths.begin(), paths.end());
  input.storage.reserve(paths.size());
  input.blobs.reserve(paths.size());
  input.catalogue.blobs.reserve(paths.size());
  for (const auto& path : paths) {
    evidence.inputComponent = "blobs/" + path.filename().string();
    const auto expected = digest(path.stem().string());
    input.storage.push_back(read(path, replay::kRenderTapeHardMaxBlobBytes - evidence.blobBytes, deadline));
    const auto& bytes = input.storage.back();
    require(replay::RenderTapeCaptureSession::sha256(bytes) == expected, "blob-digest-mismatch");
    checkDeadline(deadline);
    evidence.blobBytes += bytes.size();
    input.blobs.push_back({expected, bytes});
    input.catalogue.blobs.push_back({.digest = expected, .size = bytes.size(), .verified = 1u});
    ++evidence.blobCount;
  }
  evidence.inputComponent.clear();
  evidence.inputsAuthenticated = true;
  return input;
}

void locateFailure(Evidence& evidence, const runtime::GeneralPreflightPlan& general) {
  if (evidence.failedEvent != 0xffffffffu && evidence.failedEvent < general.tape.events.size())
    evidence.failedEventOrdinal = general.tape.event(evidence.failedEvent).header.ordinal;
}

void execute(Evidence& evidence) {
  // Pin the existing cached selectors, including zeros, before any fixture.
  // Omitted flags never inherit an unrelated process environment candidate.
  const auto drawCommandLimit = std::to_string(evidence.options.drawCommandLimit);
  require(setenv("DXMT9_DRAW_CHUNK_COMMAND_LIMIT", drawCommandLimit.c_str(), 1) == 0,
      "controlled-draw-policy-environment-unavailable");
  const auto slotHeadroom = std::to_string(evidence.options.capacityPolicy.slotHeadroomBytes);
  const auto aggregateHeadroom = std::to_string(evidence.options.capacityPolicy.aggregateHeadroomBytes);
  require(setenv("DXMT9_DIRECT_SLOT_HEADROOM_BYTES", slotHeadroom.c_str(), 1) == 0 &&
      setenv("DXMT9_DIRECT_SLOT_AGGREGATE_HEADROOM_BYTES", aggregateHeadroom.c_str(), 1) == 0,
      "controlled-capacity-policy-environment-unavailable");
  if (evidence.options.logicalSemantics || evidence.options.executionOnly) {
    // Cold route qualification observes existing production counters. Pin
    // their cached selector before preflight or any fixture/runtime effects.
    require(setenv("DXMT_PERF_COUNTERS", "1", 1) == 0 && dxmt9::perf::enabled(),
        "controlled-semantics-route-counters-unavailable");
  }
  const auto deadline = Clock::now() + std::chrono::milliseconds(evidence.options.wallMilliseconds);
  evidence.phase = "input-authentication";
  auto input = load(evidence, deadline);
  evidence.phase = "wire-preflight";
  evidence.wire = replay::validateRenderTape(input.events, input.catalogue);
  evidence.failedEvent = evidence.wire.failedEventIndex;
  require(evidence.wire.valid(), "wire-preflight-rejected");
  evidence.wireValid = true;
  evidence.phase = "general-preflight";
  runtime::GeneralPreflightPlan general;
  evidence.preflight = runtime::buildGeneralReplayPlan(input.events, input.blobs, general);
  evidence.failedEvent = evidence.preflight.failedEventIndex;
  locateFailure(evidence, general);
  require(evidence.preflight.complete(), "general-preflight-rejected");
  evidence.generalValid = true;
  require(evidence.preflight.profile == replay::kRenderTapeProfileFrame &&
      evidence.preflight.coverage.commandRecords &&
      evidence.preflight.coverage.drawPrimitiveRecords + evidence.preflight.coverage.drawIndexedPrimitiveRecords +
          evidence.preflight.coverage.drawPrimitiveUpRecords > 0u, "nonempty-general-frame-required");
  require(evidence.preflight.conservation.inputBlobs == evidence.preflight.conservation.referencedBlobs,
      "blob-conservation-preflight-rejected");
  evidence.phase = "paired-plan-authentication";
  replay::CaptureSourceReplayPlanV4 plan;
  require(replay::buildCaptureSourceReplayPlanV4(input.events, input.catalogue, input.identity,
      input.topology, plan) == replay::CaptureSourceReplayPlanV4Status::Valid, "paired-plan-rejected");
  require(plan.identity().header.authority ==
      static_cast<std::uint32_t>(replay::RenderTapeIdentityAuthority::Capture),
      "original-capture-authority-required");
  evidence.pairValid = true;
  evidence.planDigest = plan.digest();
  evidence.planEvents = plan.events().size();
  evidence.planRecords = plan.records().size();
  evidence.planSources = plan.identity().sources.size();
  require(plan.digest() == evidence.options.plan, "expected-plan-digest-mismatch");
  evidence.planDigestMatched = true;
  evidence.phase = "action-profile-authentication";
  replay::RenderTapeActionProfileV1ComponentHeader action;
  const auto& header = plan.identity().header;
  require(replay::importRenderTapeActionProfileV1Component(input.action, action) &&
      replay::renderTapeActionProfileV1ComponentMatchesBinding(action,
          header.captureToken, header.captureEpoch, header.queue,
          input.events.size(), input.identity.size(), input.topology.size(),
          evidence.options.events, evidence.options.identity, evidence.options.topology),
      "action-profile-binding-rejected");
  evidence.actionAuthenticated = true;
  evidence.actionWitness = action.witness;
  require(replay::renderTapeActionWitnessV1StableExact(action.witness),
      "stable-exact-action-witness-required");
  evidence.stableExact = true;
  evidence.phase = "original-oracle-byte-authentication";
  const auto& dimensions = evidence.preflight.requirements;
  require(dimensions.outputWidth && dimensions.outputHeight &&
      static_cast<std::uint64_t>(dimensions.outputWidth) <= componentLimit / 4u / dimensions.outputHeight,
      "oracle-dimensions-out-of-bounds");
  const auto pixels = static_cast<std::uint64_t>(dimensions.outputWidth) * dimensions.outputHeight * 4u;
  require(input.source.size() == pixels && input.output.size() == pixels,
      "original-oracle-byte-size-mismatch");
  require(general.expectedDigestValidity == replay::RenderTapeDigestValidity::Sha256 &&
      general.expectedDigest == evidence.options.output, "original-output-digest-not-bound-to-tape");
  evidence.oracleBytesAuthenticated = true;
  evidence.phase = "controlled-input-admission";
  evidence.inputAdmission.emplace(dxmt9::test::admitControlledCpuV4Input(general));
  evidence.failedEvent = evidence.inputAdmission->eventIndex();
  locateFailure(evidence, general);
  require(evidence.inputAdmission->accepted(), "controlled-v4-input-admission-rejected");
  checkDeadline(deadline);
  evidence.phase = "metal-resource-device";
  MetalDeviceLease resourceDevice;
  resourceDevice.handle = dxmt9_test_acquire_metal_device();
  require(resourceDevice.handle != NULL_OBJECT_HANDLE, "metal-resource-device-unavailable");
  evidence.resourceDeviceCreated = true;
  PresentParameters output{};
  output.backBufferWidth = general.outputDesc.width;
  output.backBufferHeight = general.outputDesc.height;
  output.backBufferFormat = replay::devicec::fmtFromD3D(general.outputDesc.format);
  output.backBufferCount = 1u;
  output.windowed = true;
  output.deviceWindow = Handle{91};
  output.presentationInterval = PresentInterval::Immediate;
  evidence.phase = "fresh-controlled-cpu-fixture";
  const bool directSlotRequested = (evidence.options.logicalSemantics || evidence.options.executionOnly) &&
      evidence.options.route == replay::ProviderReplayPolicy::ProductionSlot;
  RuntimeFixture fixture(false, false, directSlotRequested, !directSlotRequested,
      WMT::Device{resourceDevice.handle}, nullptr, &output, true);
  evidence.fixtureCreated = true;
  evidence.cpuReadyArenaSupported = fixture.routing->supportsCpuReadyArenaReplay();
  evidence.directSlotSupported = fixture.routing->supportsDirectChunkSlotReplay();
  evidence.workersDisabled = dxmt9::CommandQueueArenaLeaseTestAccess::cpuTapeWorkersDisabled(
      fixture.routing->queue_);
  evidence.offloadAbsent = !fixture.cDevice->replayOffload;
  const auto localProfile = fixture.routing->queue_.renderTapeActionProfileSnapshot();
  evidence.localConstructionProfile = localProfile.constructionProfile;
  evidence.localCurrentProfile = localProfile.currentProfile;
  evidence.localProfileRevision = localProfile.revision;
  evidence.localExactProfile = localProfile.valid &&
      localProfile.constructionProfile == dxmt9::kRenderTapeActionProfileExact &&
      localProfile.currentProfile == dxmt9::kRenderTapeActionProfileExact && !localProfile.revision;
  require(evidence.cpuReadyArenaSupported == !directSlotRequested &&
      evidence.directSlotSupported == directSlotRequested &&
      evidence.workersDisabled && evidence.offloadAbsent && evidence.localExactProfile,
      "controlled-inventory-fixed-fixture-setup-rejected");
  Executor executor(fixture, general, input.blobs);
  Executor::ControlledV4InventoryOptions options;
  options.executionMode = evidence.options.executionOnly
      ? dxmt9::test::CpuGeneralTapeControlledV4ExecutionMode::ExecutionOnly
      : dxmt9::test::CpuGeneralTapeControlledV4ExecutionMode::Observe;
  options.route = evidence.options.route;
  options.sourcePlan = &plan;
  options.exactTape = input.events;
  options.deadline = deadline;
  options.observeLogicalSemantics = evidence.options.logicalSemantics;
  options.legalCandidateNoOverwriteContract = evidence.options.legalCandidateNoOverwriteContract;
  // This exact fixture was freshly constructed above with auto-depth disabled.
  // RT0 still requires original canonical provenance, never a native alias.
  options.freshNoAutoDepthSurfaceDefaults = !output.enableAutoDepthStencil;
  options.producerContract = evidence.options.producerContract == "pe-present-reservation-v1"
      ? replay::RenderTapeExecutionV4ProducerContract::PePresentReservationV1
      : replay::RenderTapeExecutionV4ProducerContract::None;
  evidence.phase = evidence.options.executionOnly ? "controlled-execution-only"
      : evidence.options.logicalSemantics ? "controlled-logical-semantics" : "controlled-metadata-inventory";
  evidence.result.emplace(executor.runControlledV4Inventory(options));
  evidence.failedEvent = evidence.result->failedEvent;
  locateFailure(evidence, general);
  evidence.authorityScopeValid = !evidence.result->fullSemanticOracleComplete &&
      !evidence.result->logicalSemantics.fullLogicalSemanticOracleComplete &&
      !evidence.result->logicalSemantics.originalSemanticEquivalenceQualified &&
      !evidence.result->logicalSemantics.crossPolicyEquivalenceQualified &&
      !evidence.result->logicalSemantics.gpuResultOracle &&
      !evidence.result->logicalSemantics.policyQualified &&
      !evidence.result->logicalSemantics.timingEligible &&
      !evidence.result->logicalSemantics.futurePreservationQualified &&
      !evidence.result->logicalSemantics.fullShaderFetchCoverageQualified &&
      !evidence.result->actualMetal && !evidence.result->gpuResultOracle &&
      !evidence.result->policyQualified && !evidence.result->timingEligible &&
      !evidence.result->consumer.payloadDigestPresent &&
      !evidence.result->consumer.fullSemanticOracleComplete &&
      !evidence.result->consumer.actualMetal && !evidence.result->consumer.gpuResultOracle &&
      !evidence.result->consumer.policyQualified && !evidence.result->consumer.timingEligible &&
      !evidence.result->queryReplay.actualMetal && !evidence.result->queryReplay.gpuResultOracle &&
      !evidence.result->provenance.policyQualified && !evidence.result->provenance.timingEligible &&
      !evidence.result->provenance.pixelsQualified;
  if (evidence.options.executionOnly) {
    evidence.authorityScopeValid = evidence.authorityScopeValid &&
        !evidence.result->metadataInventoryComplete && !evidence.result->logicalSemantics.requested &&
        !evidence.result->logicalSemantics.accepted && !evidence.result->logicalSemantics.complete &&
        !evidence.result->logicalSemantics.logicalStreamComplete;
  }
  evidence.phase = "fixture-teardown";
  fixture.releaseCoreForInspection();
  evidence.teardownComplete = true;
  require(evidence.authorityScopeValid, "controlled-inventory-authority-scope-violation");
  require(evidence.result->complete &&
      (evidence.options.executionOnly ? evidence.result->executionOnly.complete
                                     : evidence.result->metadataInventoryComplete) &&
      evidence.result->resetOk && evidence.result->failure.empty(),
      evidence.options.executionOnly ? "controlled-execution-only-incomplete"
                                    : "controlled-metadata-inventory-incomplete");
  if (evidence.options.logicalSemantics) {
    require(evidence.result->logicalSemantics.accepted && evidence.result->logicalSemantics.complete,
        "controlled-logical-semantics-incomplete");
  }
  if (evidence.options.logicalSemantics || evidence.options.executionOnly) {
    const auto& coverage = evidence.result->routeCoverage;
    require(coverage.policySelected && coverage.observed && coverage.monotonic && coverage.complete &&
        coverage.requestedRoute == evidence.options.route && coverage.effectiveRoute == evidence.options.route,
        "controlled-logical-semantics-route-coverage-incomplete");
  }
  if (evidence.options.executionOnly) {
    const auto& result = *evidence.result;
    const auto& omitted = result.executionOnly;
    require(result.executionMode == dxmt9::test::CpuGeneralTapeControlledV4ExecutionMode::ExecutionOnly &&
        omitted.requested && omitted.complete && omitted.lifecycleCoverageComplete &&
        omitted.resourceVersionsOmitted && omitted.recordContextsOmitted && omitted.expectedImagesOmitted &&
        omitted.canonicalDrawCopiesOmitted && omitted.payloadObserverOmitted && omitted.sourceRowReadsOmitted &&
        omitted.logicalHashingOmitted && omitted.diagnosticMutationReceiptsOmitted &&
        omitted.capturedBindingImagesOmitted && omitted.uploadByteComparisonsOmitted &&
        result.bufferVersions.empty() && result.inventory.empty() && result.canonicalDrawRecords.empty() &&
        !result.recordContexts && !result.bufferVersionsObserved,
        "controlled-execution-only-omission-evidence-incomplete");
    const auto& consumer = result.consumer;
    const auto& state = consumer.state;
    require(consumer.workerExited && consumer.cleanupComplete && !consumer.cleanupFailed &&
        state.producerClosed && state.phase == dxmt9::test::CpuV4ConsumerPhase::Stopped &&
        state.sourcePhase == dxmt9::test::CpuV4ConsumerSourcePhase::Empty &&
        omitted.sources == state.observed && omitted.commands == state.commands && omitted.draws == result.draws &&
        omitted.submitted == state.submitted && omitted.acknowledged == state.acknowledged &&
        omitted.retired == state.retired && omitted.sources == omitted.submitted &&
        omitted.submitted == omitted.acknowledged && omitted.acknowledged == omitted.retired,
        "controlled-execution-only-lifecycle-evidence-incomplete");
    const auto completeClock = [](const auto& scope) {
      return scope.started && scope.valid && scope.complete;
    };
    require(completeClock(result.clocks.setup) && completeClock(result.clocks.body) &&
        completeClock(result.clocks.teardown), "controlled-execution-only-clock-evidence-incomplete");
  }
  evidence.phase = evidence.options.executionOnly ? "execution-only-complete"
      : evidence.options.logicalSemantics ? "logical-semantics-complete" : "metadata-inventory-complete";
}

void printIdentity(const dxmt9::test::CpuV4ConsumerIdentity& value) {
  std::cout << "{\"queue\":" << value.queue << ",\"capture_token\":" << value.captureToken
      << ",\"capture_epoch\":" << value.captureEpoch << ",\"sequence\":" << value.sequence
      << ",\"source_index\":" << value.sourceIndex << ",\"source_generation\":" << value.sourceGeneration
      << ",\"storage_first_page\":" << value.storageFirstPage
      << ",\"storage_page_count\":" << value.storagePageCount
      << ",\"storage_generation\":" << value.storageGeneration << '}';
}

const char* bufferVersionStatusName(dxmt9::resources::BufferVersionObservationStatus status) noexcept {
  using Status = dxmt9::resources::BufferVersionObservationStatus;
  switch (status) {
  case Status::Valid: return "valid";
  case Status::InvalidRequest: return "invalid-request";
  case Status::MissingBuffer: return "missing-buffer";
  case Status::BackingUnavailable: return "backing-unavailable";
  case Status::InvalidBacking: return "invalid-backing";
  case Status::InspectionFailed: return "inspection-failed";
  }
  return "unknown";
}

const char* resourceContextStatusName(dxmt9::test::CpuTapeResourceContextStatus status) noexcept {
  using Status = dxmt9::test::CpuTapeResourceContextStatus;
  switch (status) {
  case Status::Accepted: return "accepted";
  case Status::InvalidInput: return "invalid-input";
  case Status::CapacityExceeded: return "capacity-exceeded";
  case Status::Conflict: return "conflict";
  case Status::AllocationFailed: return "allocation-failed";
  }
  return "unknown";
}

void printWireIdentity(const D9CWireObjectIdentity& identity) {
  std::cout << "{\"kind\":" << identity.kind << ",\"generation\":" << identity.generation
      << ",\"object_id\":" << identity.objectId << '}';
}

std::string logicalDigestHex(std::uint64_t value) {
  constexpr char digits[] = "0123456789abcdef";
  std::string out(16u, '0');
  for (std::size_t i = 0u; i < out.size(); ++i) {
    out[out.size() - i - 1u] = digits[value & 15u];
    value >>= 4u;
  }
  return out;
}

void printLogicalSemantics(const dxmt9::test::CpuGeneralTapeControlledV4LogicalSemanticResult& value) {
  std::cout << "{\"requested\":" << boolean(value.requested)
      << ",\"admitted\":" << boolean(value.admitted)
      << ",\"accepted\":" << boolean(value.accepted)
      << ",\"complete\":" << boolean(value.complete)
      << ",\"legal_candidate_nooverwrite_contract\":" << boolean(value.legalCandidateNoOverwriteContract)
      << ",\"conditional_on_legal_nooverwrite\":" << boolean(value.conditionalOnLegalNoOverwrite)
      << ",\"logical_stream_complete\":" << boolean(value.logicalStreamComplete)
      << ",\"full_logical_semantic_oracle_complete\":" << boolean(value.fullLogicalSemanticOracleComplete)
      << ",\"original_semantic_equivalence_qualified\":" << boolean(value.originalSemanticEquivalenceQualified)
      << ",\"cross_policy_equivalence_qualified\":" << boolean(value.crossPolicyEquivalenceQualified)
      << ",\"gpu_result_oracle\":" << boolean(value.gpuResultOracle)
      << ",\"policy_qualified\":" << boolean(value.policyQualified)
      << ",\"timing_eligible\":" << boolean(value.timingEligible)
      << ",\"future_preservation_qualified\":" << boolean(value.futurePreservationQualified)
      << ",\"full_shader_fetch_coverage_qualified\":" << boolean(value.fullShaderFetchCoverageQualified)
      << ",\"failure\":" << quoted(value.failure)
      << ",\"sources\":" << value.sources << ",\"commands\":" << value.commands
      << ",\"draws\":" << value.draws << ",\"original_items\":" << value.originalItems
      << ",\"source_rows_required\":" << value.sourceRowsRequired
      << ",\"source_rows_qualified\":" << value.sourceRowsQualified
      << ",\"logical_buffer_hash_bytes\":" << value.logicalBufferHashBytes
      << ",\"digest_algorithm\":\"fnv1a64-logical-v4\",\"digest\":" << quoted(logicalDigestHex(value.digest))
      << ",\"receipts\":[";
  for (std::size_t i = 0u; i < value.receipts.size(); ++i) {
    if (i) std::cout << ',';
    const auto& item = value.receipts[i];
    std::cout << "{\"record\":{\"event_ordinal\":" << item.record.eventOrdinal
        << ",\"record_index\":" << item.record.recordIndex << '}'
        << ",\"command_kind\":" << static_cast<unsigned>(item.commandKind)
        << ",\"identity_command_kind\":" << dxmt9::render::tapeTopologyCommandKind(item.commandKind)
        << ",\"sequence\":" << item.sequence
        << ",\"physical_command_index\":" << item.physicalCommandIndex
        << ",\"subdraw_index\":";
    if (item.subdrawIndex == UINT32_MAX) std::cout << "null"; else std::cout << item.subdrawIndex;
    std::cout << ",\"digest\":" << quoted(logicalDigestHex(item.digest))
        << ",\"serialized\":" << boolean(item.serialized)
        << ",\"failure\":" << quoted(item.failure)
        << ",\"source_rows_required\":" << boolean(item.sourceRowsRequired)
        << ",\"source_rows\":{\"status\":" << quoted(std::string_view(
            dxmt9::test::cpuTapeSourceObservationStatusName(item.sourceRows.status)))
        << ",\"qualified\":" << boolean(item.sourceRows.qualified())
        << ",\"source_sequence\":" << item.sourceRows.sourceSequence
        << ",\"completed_sequence\":" << item.sourceRows.completedSequence
        << ",\"planned_read_bytes\":" << item.sourceRows.plannedReadBytes
        << ",\"proved\":" << boolean(item.sourceRows.readProof && item.sourceRows.readProof->proved())
        << ",\"proved_bytes\":" << (item.sourceRows.readProof ? item.sourceRows.readProof->provedBytes : 0u)
        << "}}";
  }
  std::cout << "]}";
}

void printClockScope(const dxmt9::test::CpuGeneralTapeControlledV4ClockScope& value) {
  std::cout << "{\"started\":" << boolean(value.started)
      << ",\"valid\":" << boolean(value.valid) << ",\"complete\":" << boolean(value.complete)
      << ",\"wall_ns\":" << value.wallNs << ",\"process_cpu_ns\":" << value.processCpuNs << '}';
}

void printExecutionOnly(const Result& result) {
  const auto& value = result.executionOnly;
  std::cout << ",\"execution_mode\":\"execution-only\",\"execution_only\":{\"requested\":"
      << boolean(value.requested) << ",\"complete\":" << boolean(value.complete)
      << ",\"lifecycle_coverage_complete\":" << boolean(value.lifecycleCoverageComplete)
      << ",\"resource_versions_omitted\":" << boolean(value.resourceVersionsOmitted)
      << ",\"record_contexts_omitted\":" << boolean(value.recordContextsOmitted)
      << ",\"expected_images_omitted\":" << boolean(value.expectedImagesOmitted)
      << ",\"canonical_draw_copies_omitted\":" << boolean(value.canonicalDrawCopiesOmitted)
      << ",\"payload_observer_omitted\":" << boolean(value.payloadObserverOmitted)
      << ",\"source_row_reads_omitted\":" << boolean(value.sourceRowReadsOmitted)
      << ",\"logical_hashing_omitted\":" << boolean(value.logicalHashingOmitted)
      << ",\"diagnostic_mutation_receipts_omitted\":" << boolean(value.diagnosticMutationReceiptsOmitted)
      << ",\"captured_binding_images_omitted\":" << boolean(value.capturedBindingImagesOmitted)
      << ",\"upload_byte_comparisons_omitted\":" << boolean(value.uploadByteComparisonsOmitted)
      << ",\"sources\":" << value.sources << ",\"commands\":" << value.commands
      << ",\"draws\":" << value.draws << ",\"submitted\":" << value.submitted
      << ",\"acknowledged\":" << value.acknowledged << ",\"retired\":" << value.retired << '}'
      << ",\"clocks\":{\"clock_domain\":\"steady-wall-and-process-cpu\""
      << ",\"process_cpu_clock\":\"CLOCK_PROCESS_CPUTIME_ID\",\"scope\":\"executor-run-only\""
      << ",\"body_scope\":\"after-setup-and-arm-through-consumer-join\""
      << ",\"body_excludes_setup_and_teardown\":true,\"setup\":";
  printClockScope(result.clocks.setup);
  std::cout << ",\"body\":";
  printClockScope(result.clocks.body);
  std::cout << ",\"teardown\":";
  printClockScope(result.clocks.teardown);
  std::cout << '}';
}

void printResult(const Result& result) {
  const auto& consumer = result.consumer;
  const auto& state = consumer.state;
  std::cout << "{\"complete\":" << boolean(result.complete)
      << ",\"metadata_inventory_complete\":" << boolean(result.metadataInventoryComplete)
      << ",\"runtime_budgeted_attempt\":" << boolean(result.runtimeBudgetedAttempt)
      << ",\"source_progress_complete\":" << boolean(result.sourceProgressComplete)
      << ",\"reset_ok\":" << boolean(result.resetOk) << ",\"failure\":" << quoted(result.failure)
      << ",\"logical_semantics\":";
  printLogicalSemantics(result.logicalSemantics);
  if (result.executionOnly.requested) printExecutionOnly(result);
  if (result.logicalSemantics.requested || result.executionOnly.requested) {
    const auto& coverage = result.routeCoverage;
    std::cout << ",\"route_coverage\":{\"requested_route\":"
        << quoted(std::string_view(routeName(coverage.requestedRoute)))
        << ",\"effective_route\":";
    if (coverage.policySelected)
      std::cout << quoted(std::string_view(routeName(coverage.effectiveRoute)));
    else std::cout << "null";
    std::cout << ",\"policy_selected\":" << boolean(coverage.policySelected)
        << ",\"observed\":" << boolean(coverage.observed)
        << ",\"monotonic\":" << boolean(coverage.monotonic)
        << ",\"complete\":" << boolean(coverage.complete)
        << ",\"direct_leases\":" << coverage.directLeases
        << ",\"direct_draws\":" << coverage.directDraws
        << ",\"compatibility_draws\":" << coverage.compatibilityDraws
        << ",\"compatibility_batches\":" << coverage.compatibilityBatches
        << ",\"compatibility_unbatched_draws\":" << coverage.compatibilityUnbatchedDraws
        << ",\"fallback_draws\":" << coverage.fallbackDraws << '}';
  }
  std::cout << ",\"failed_event\":";
  if (result.failedEvent == 0xffffffffu) std::cout << "null"; else std::cout << result.failedEvent;
  std::cout << ",\"resource_version_failure\":";
  if (result.resourceVersionFailure) {
    const auto& failure = *result.resourceVersionFailure;
    std::cout << "{\"stage\":";
    if (failure.stage) std::cout << quoted(std::string_view(failure.stage)); else std::cout << "null";
    std::cout << ",\"requested_identity\":";
    printWireIdentity(failure.requestedIdentity);
    std::cout << ",\"normalized_identity\":";
    printWireIdentity(failure.normalizedIdentity);
    std::cout << ",\"native_kind\":" << failure.nativeKind
        << ",\"native_handle\":" << failure.nativeHandle.value
        << ",\"prior_logical_content_epoch\":" << failure.priorLogicalContentEpoch
        << ",\"status\":" << quoted(std::string_view(resourceContextStatusName(failure.status)))
        << ",\"status_value\":" << static_cast<unsigned>(failure.status)
        << ",\"installed_native_owners\":[";
    for (std::size_t i = 0u; i < failure.installedNativeOwners.size(); ++i) {
      if (i) std::cout << ',';
      printWireIdentity(failure.installedNativeOwners[i]);
    }
    std::cout << "]}";
  } else std::cout << "null";
  std::cout << ",\"counters\":{\"events\":" << result.events << ",\"commands\":" << result.commands
      << ",\"draws\":" << result.draws << ",\"resources_created\":" << result.resourcesCreated
      << ",\"resources_released\":" << result.resourcesReleased
      << ",\"observed_mutations\":" << result.observedMutations
      << ",\"observed_texture_uploads\":" << result.observedTextureUploads
      << ",\"record_contexts\":" << result.recordContexts
      << ",\"setup_submissions\":" << result.setupSubmissions
      << ",\"setup_acknowledgements\":" << result.setupAcknowledgements
      << ",\"buffer_versions_observed\":" << result.bufferVersionsObserved
      << ",\"buffer_versions_unavailable\":" << result.bufferVersionsUnavailable << '}'
      << ",\"consumer\":{\"phase\":" << static_cast<unsigned>(state.phase)
      << ",\"failure\":" << static_cast<unsigned>(consumer.failure)
      << ",\"completion_policy\":\"immediate-fifo-cpu-acknowledgement\""
      << ",\"observed\":" << state.observed << ",\"submitted\":" << state.submitted
      << ",\"acknowledged\":" << state.acknowledged << ",\"retired\":" << state.retired
      << ",\"commands\":" << state.commands << ",\"lifecycle_actions\":" << state.lifecycleActions
      << ",\"receipts\":" << consumer.receipts << ",\"worker_exited\":" << boolean(consumer.workerExited)
      << ",\"producer_closed\":" << boolean(state.producerClosed)
      << ",\"cleanup_complete\":" << boolean(consumer.cleanupComplete)
      << ",\"cleanup_failed\":" << boolean(consumer.cleanupFailed)
      << ",\"active_identity\":";
  printIdentity(state.active);
  std::cout << ",\"limits\":{\"max_sources\":" << state.limits.maxSources
      << ",\"max_commands\":" << state.limits.maxCommands
      << ",\"max_lifecycle_actions\":" << state.limits.maxLifecycleActions << "}}"
      << ",\"expected_buffer_budget\":{\"limit_bytes\":"
      << result.expectedBufferBudget.maxRetainedExpectedBufferBytes
      << ",\"charged_bytes\":" << result.expectedBufferBudget.chargedExpectedBufferBytes
      << ",\"high_water_bytes\":" << result.expectedBufferBudget.highWaterExpectedBufferBytes
      << ",\"physical_byte_proof\":false}"
      << ",\"initial_setup\":{\"owners\":" << result.initialSetup.owners
      << ",\"seeds\":" << result.initialSetup.seeds << ",\"aliases\":" << result.initialSetup.aliases
      << ",\"created\":" << result.initialSetup.created << ",\"prepared\":" << result.initialSetup.prepared
      << ",\"activated\":" << result.initialSetup.activated
      << ",\"staged_seeds\":" << result.initialSetup.stagedSeeds
      << ",\"consumed_seeds\":" << result.initialSetup.consumedSeeds
      << ",\"released_hidden\":" << result.initialSetup.releasedHidden
      << ",\"seed_bytes\":" << result.initialSetup.seedBytes
      << ",\"allocation_bytes\":" << result.initialSetup.allocationBytes
      << ",\"host_checkpoint_joined\":" << boolean(result.initialSetup.hostCheckpointJoined) << '}'
      << ",\"provenance\":{\"canonical_traversal_complete\":"
      << boolean(result.controlledCpu.canonicalTraversalComplete)
      << ",\"raw_ack_coverage_complete\":" << boolean(result.controlledCpu.rawAckCoverageComplete)
      << ",\"canonical_origin_mapping_complete\":" << boolean(result.controlledCpu.semanticMappingComplete)
      << ",\"controlled_cpu_complete\":" << boolean(result.controlledCpu.controlledCpuComplete)
      << ",\"replayed_events\":" << result.provenance.replayedEvents
      << ",\"dispatched_records\":" << result.provenance.dispatchedRecords
      << ",\"acknowledged_raws\":" << result.provenance.acknowledgedRaws
      << ",\"source_obligations_registered\":" << result.controlledCpu.sourceObligationsRegistered
      << ",\"source_obligations_qualified\":" << result.controlledCpu.sourceObligationsQualified
      << ",\"present_preparation_attempted\":" << boolean(result.provenance.presentPreparationAttempted)
      << ",\"present_preparation_completed\":" << boolean(result.provenance.presentPreparationCompleted) << '}'
      << ",\"query_projection\":{\"present\":" << boolean(result.queryReplay.present)
      << ",\"definitions\":" << result.queryReplay.queryDefinitions
      << ",\"issues\":" << result.queryReplay.issueRecords
      << ",\"recorded_polls\":" << result.queryReplay.recordedPolls
      << ",\"replayed_polls\":" << result.queryReplay.replayedPolls
      << ",\"checkpoints\":" << result.queryReplay.checkpointDefinitions
      << ",\"rows\":" << result.queryReplay.rows.size() << ",\"gpu_result_oracle\":false}"
      << ",\"buffer_version_projection\":{\"scope\":\"producer-instantaneous-current-allocation-metadata\""
      << ",\"logical_epoch_authenticated_by_revision\":false,\"backing_retained\":false"
      << ",\"old_ring_epoch_proven\":false,\"physical_byte_proof\":false,\"rows\":[";
  for (std::size_t i = 0u; i < result.bufferVersions.size(); ++i) {
    if (i) std::cout << ',';
    const auto& row = result.bufferVersions[i];
    const auto& observed = row.observation;
    std::cout << "{\"identity\":{\"kind\":" << row.identity.kind
        << ",\"generation\":" << row.identity.generation << ",\"object_id\":" << row.identity.objectId << '}'
        << ",\"logical_content_epoch\":" << row.logicalContentEpoch
        << ",\"status\":" << quoted(std::string_view(bufferVersionStatusName(observed.status)))
        << ",\"observed\":" << boolean(observed.observed())
        << ",\"local_buffer_handle\":";
    if (observed.observed()) std::cout << observed.buffer.value; else std::cout << "null";
    std::cout << ",\"local_byte_size\":";
    if (observed.observed()) std::cout << observed.byteSize; else std::cout << "null";
    std::cout << ",\"local_content_revision\":";
    if (observed.observed()) std::cout << observed.contentRevision; else std::cout << "null";
    std::cout << ",\"has_versioned_backing\":";
    if (observed.observed()) std::cout << boolean(observed.hasVersionedBacking); else std::cout << "null";
    std::cout << ",\"current_allocation\":";
    if (observed.observed()) {
      std::cout << "{\"metal_handle\":" << observed.currentBacking.metalHandle
          << ",\"byte_size\":" << observed.currentBacking.byteSize << '}';
    } else std::cout << "null";
    std::cout << '}';
  }
  std::cout << "]},\"consumer_receipts\":[";
  for (std::size_t i = 0u; i < result.consumerReceipts.size(); ++i) {
    if (i) std::cout << ',';
    const auto& receipt = result.consumerReceipts[i];
    std::cout << "{\"identity\":";
    if (receipt.observation) printIdentity(receipt.observation->identity); else std::cout << "null";
    std::cout << ",\"submitted\":" << boolean(receipt.submitted)
        << ",\"acknowledged\":" << boolean(receipt.acknowledged)
        << ",\"retired\":" << boolean(receipt.retired)
        << ",\"cleanup_complete\":" << boolean(receipt.cleanupComplete) << '}';
  }
  std::size_t rowQualified = 0u, rowNotQualified = 0u;
  for (const auto& source : result.inventory)
    for (const auto& draw : source.draws)
      draw.sourceRows.qualified() ? ++rowQualified : ++rowNotQualified;
  std::cout << "],\"source_row_observation\":{\"scope\":\"coherent-instantaneous-full-stride-rows\""
      << ",\"qualified_draws\":" << rowQualified << ",\"not_qualified_draws\":" << rowNotQualified
      << ",\"shader_demand_complete\":false,\"future_preservation\":false,\"physical_fetch_complete\":false}"
      << ",\"resource_context_projection\":\"presence-and-entry-count-only\",\"sources\":[";
  for (std::size_t i = 0u; i < result.inventory.size(); ++i) {
    if (i) std::cout << ',';
    const auto& source = result.inventory[i];
    const auto& publication = source.publication;
    std::cout << "{\"source_index\":" << publication.source.id.index
        << ",\"source_generation\":" << publication.source.id.generation
        << ",\"storage_first_page\":" << publication.source.storage.firstPage
        << ",\"storage_page_count\":" << publication.source.storage.pageCount
        << ",\"storage_generation\":" << publication.source.storage.generation
        << ",\"sequence\":" << publication.metadata.seqId
        << ",\"raw_ordinal\":" << publication.metadata.rawOrdinal
        << ",\"source_ordinal\":" << publication.metadata.sourceOrdinal
        << ",\"used_bytes\":" << publication.metadata.usedBytes
        << ",\"has_present\":" << boolean(publication.hasPresent) << ",\"commands\":[";
    for (std::size_t c = 0u; c < source.commands.size(); ++c) {
      if (c) std::cout << ',';
      const auto& command = source.commands[c];
      std::cout << "{\"physical_command\":" << command.physicalCommandIndex
          << ",\"kind\":" << command.kind << ",\"draw_count\":" << command.drawCount << '}';
    }
    std::cout << "],\"origins\":[";
    for (std::size_t o = 0u; o < source.origins.size(); ++o) {
      if (o) std::cout << ',';
      const auto& row = source.origins[o];
      std::cout << "{\"physical_command\":" << row.origin.physicalCommandIndex
          << ",\"kind\":" << row.origin.kind << ",\"first_subdraw\":" << row.origin.firstSubdraw
          << ",\"subdraw_count\":" << row.origin.subdrawCount
          << ",\"event_ordinal\":" << row.origin.record.eventOrdinal
          << ",\"record_index\":" << row.origin.record.recordIndex
          << ",\"context_present\":" << boolean(bool(row.context))
          << ",\"context_entries\":" << (row.context ? row.context->entries().size() : 0u) << '}';
    }
    std::cout << "],\"draws\":[";
    for (std::size_t d = 0u; d < source.draws.size(); ++d) {
      if (d) std::cout << ',';
      const auto& draw = source.draws[d];
      const auto& param = draw.parameters;
      std::cout << "{\"physical_command\":" << draw.physicalCommandIndex
          << ",\"subdraw\":" << draw.subdrawIndex << ",\"event_ordinal\":" << draw.record.eventOrdinal
          << ",\"record_index\":" << draw.record.recordIndex
          << ",\"primitive_type\":" << static_cast<unsigned>(param.primitiveType)
          << ",\"primitive_count\":" << param.primitiveCount << ",\"indexed\":" << boolean(param.indexed)
          << ",\"start_vertex\":" << param.startVertex << ",\"base_vertex\":" << param.baseVertexIndex
          << ",\"start_index\":" << param.startIndex << ",\"index_type\":" << static_cast<unsigned>(param.indexType)
          << ",\"instance_count\":" << param.instanceCount << ",\"stream_mask\":" << draw.streamMask
          << ",\"index_buffer\":" << draw.indexBuffer.value
          << ",\"user_vertex_bytes\":" << draw.userVertexBytes << ",\"user_index_bytes\":" << draw.userIndexBytes
          << ",\"binding_override\":" << boolean(draw.hasBindingOverride)
          << ",\"binding_snapshot\":" << boolean(draw.hasBindingSnapshot) << ",\"streams\":[";
      for (std::size_t s = 0u; s < draw.streamBuffers.size(); ++s) {
        if (s) std::cout << ',';
        std::cout << "{\"stream\":" << s << ",\"buffer\":" << draw.streamBuffers[s].value
            << ",\"offset\":" << draw.streamOffsets[s] << ",\"stride\":" << draw.streamStrides[s]
            << ",\"frequency\":" << draw.streamFrequencies[s] << '}';
      }
      const auto& rows = draw.sourceRows;
      std::cout << "],\"source_rows\":{\"status\":"
          << quoted(std::string_view(dxmt9::test::cpuTapeSourceObservationStatusName(rows.status)))
          << ",\"qualified\":" << boolean(rows.qualified())
          << ",\"source_sequence\":" << rows.sourceSequence
          << ",\"completed_sequence\":" << rows.completedSequence
          << ",\"effective_indexed\":" << boolean(rows.effectiveIndexed)
          << ",\"source_base\":" << rows.sourceBase << ",\"source_stride\":" << rows.sourceStride
          << ",\"source_byte_size\":" << rows.sourceByteSize << ",\"planner_status\":";
      if (rows.plannerStatus) std::cout << static_cast<unsigned>(*rows.plannerStatus);
      else std::cout << "null";
      std::cout << ",\"qualified_primitive_count\":" << rows.qualifiedPrimitiveCount
          << ",\"source_element_count\":" << rows.sourceElementCount
          << ",\"unique_vertex_rows\":" << rows.uniqueVertexRows
          << ",\"vertex_range_count\":" << rows.vertexRangeCount
          << ",\"planned_read_bytes\":" << rows.plannedReadBytes << ",\"index_range\":";
      if (rows.indexRange)
        std::cout << "{\"offset\":" << rows.indexRange->byteOffset
            << ",\"size\":" << rows.indexRange->byteSize << '}';
      else std::cout << "null";
      std::cout << ",\"read_proof\":";
      if (rows.readProof) {
        const auto& proof = *rows.readProof;
        std::cout << "{\"status\":" << static_cast<unsigned>(proof.status)
            << ",\"proved\":" << boolean(proof.proved()) << ",\"failed_request\":";
        if (proof.failedRequest == std::numeric_limits<std::uint32_t>::max()) std::cout << "null";
        else std::cout << proof.failedRequest;
        std::cout << ",\"failed_range\":";
        if (proof.failedRange == std::numeric_limits<std::uint32_t>::max()) std::cout << "null";
        else std::cout << proof.failedRange;
        std::cout << ",\"proved_requests\":" << proof.provedRequests
            << ",\"proved_ranges\":" << proof.provedRanges
            << ",\"proved_bytes\":" << proof.provedBytes << '}';
      } else std::cout << "null";
      std::cout << "}}";
    }
    std::cout << "]}";
  }
  std::cout << "]}";
}

void printStageEnvironment() {
  constexpr std::array<std::string_view, 11u> names{
      "DXMT9_DISABLE_PRESENT_BOUNDARY", "DXMT9_SYNC_PRESENT_FLUSH", "DXMT9_OFFLOAD_COMMIT_REPLAY",
      "DXMT9_DRAW_CHUNK_COMMAND_LIMIT", "DXMT9_CHUNK_DRAW_PAYLOAD_ARENA_LIMIT_BYTES",
      "DXMT9_CPU_READY_TAPE", "DXMT9_CPU_READY_EARLY_PREFIX", "DXMT9_SPLIT_STRETCH_CHUNK",
      "DXMT_PERF_COUNTERS", "DXMT9_DIRECT_SLOT_HEADROOM_BYTES", "DXMT9_DIRECT_SLOT_AGGREGATE_HEADROOM_BYTES"};
  std::cout << '{';
  for (std::size_t index = 0u; index < names.size(); ++index) {
    if (index) std::cout << ',';
    std::cout << quoted(names[index]) << ':';
    const auto* value = std::getenv(names[index].data());
    std::size_t length = 0u;
    if (value) while (length <= 16u && value[length]) ++length;
    if (value && length <= 16u) std::cout << quoted(std::string_view(value, length));
    else std::cout << "null";
  }
  std::cout << '}';
}

void print(const Evidence& evidence) {
  const auto complete = evidence.failure.empty() && evidence.result && evidence.result->complete &&
      (evidence.options.executionOnly ? evidence.result->executionOnly.complete
                                     : evidence.result->metadataInventoryComplete) &&
      evidence.teardownComplete && evidence.authorityScopeValid &&
      (!evidence.options.logicalSemantics || evidence.result->logicalSemantics.accepted);
  std::cout << "{\"schema\":" << quoted(evidence.options.executionOnly
          ? std::string_view("dxmt9.controlled-cpu-v4-execute.v1")
          : evidence.options.logicalSemantics
          ? std::string_view("dxmt9.controlled-cpu-v4-semantics.v1")
          : std::string_view("dxmt9.controlled-cpu-v4-inventory.v1"))
      << ",\"mode\":" << quoted(evidence.options.executionOnly
          ? std::string_view("controlled-cpu-v4-execute")
          : evidence.options.logicalSemantics
          ? std::string_view("controlled-cpu-v4-semantics")
          : std::string_view("controlled-cpu-v4-inventory"))
      << ",\"complete\":" << boolean(complete) << ",\"phase\":" << quoted(evidence.phase)
      << ",\"failure\":" << quoted(evidence.failure) << ",\"failed_input_component\":";
  if (evidence.inputComponent.empty()) std::cout << "null";
  else std::cout << quoted(evidence.inputComponent);
  std::cout << ",\"failed_event\":";
  if (evidence.failedEvent == 0xffffffffu) std::cout << "null"; else std::cout << evidence.failedEvent;
  std::cout << ",\"failed_event_ordinal\":";
  if (evidence.failedEventOrdinal) std::cout << evidence.failedEventOrdinal; else std::cout << "null";
  std::cout << ",\"capacity_policy\":{\"slot_headroom_bytes\":"
      << evidence.options.capacityPolicy.slotHeadroomBytes
      << ",\"aggregate_headroom_bytes\":" << evidence.options.capacityPolicy.aggregateHeadroomBytes << '}';
  std::cout << ",\"replay_policy\":{\"schema\":\"dxmt9.render_tape.v4_replay_policy.v1\",\"route\":"
      << quoted(std::string_view(routeName(evidence.options.route)))
      << ",\"draw_command_limit\":" << evidence.options.drawCommandLimit
      << ",\"slot_headroom_bytes\":" << evidence.options.capacityPolicy.slotHeadroomBytes
      << ",\"aggregate_headroom_bytes\":" << evidence.options.capacityPolicy.aggregateHeadroomBytes << '}';
  if (evidence.options.logicalSemantics || evidence.options.executionOnly) {
    std::cout << ",\"requested_route\":" << quoted(std::string_view(routeName(evidence.options.route)))
        << ",\"effective_route\":";
    if (evidence.result && evidence.result->routeCoverage.policySelected)
      std::cout << quoted(std::string_view(routeName(evidence.result->routeCoverage.effectiveRoute)));
    else std::cout << "null";
    std::cout << ",\"effective_stage_environment\":";
    printStageEnvironment();
  }
  std::cout << ",\"full_semantic_oracle_complete\":false,\"actual_metal\":false,\"gpu_replay\":false"
      << ",\"diagnostic\":true,\"public_v4_execution_enabled\":false"
      << ",\"future_backing_preservation_complete\":false"
      << ",\"buffer_preservation_contract\":" << quoted(evidence.options.legalCandidateNoOverwriteContract
          ? std::string_view("candidate-legal-nooverwrite-closed-writers-v1") : std::string_view("none"))
      << ",\"gpu_result_oracle\":false,\"pixel_readback\":false,\"policy_qualified\":false,\"timing_eligible\":false"
      << ",\"physical_fetch_oracle_complete\":false,\"input_admission_profile\":\"controlled-v4-raw256\""
      << ",\"max_wall_ms\":" << evidence.options.wallMilliseconds
      << ",\"iterations\":" << (evidence.fixtureCreated ? 1u : 0u)
      << ",\"producer_contract\":" << quoted(evidence.options.producerContract)
      << ",\"native_producer_attestation_authenticated\":false,\"requires_launcher_pe_attestation\":"
      << boolean(evidence.options.producerContract == "pe-present-reservation-v1")
      << ",\"authentication\":{\"owned_inputs_authenticated\":" << boolean(evidence.inputsAuthenticated)
      << ",\"wire_valid\":" << boolean(evidence.wireValid)
      << ",\"general_valid\":" << boolean(evidence.generalValid) << ",\"pair_valid\":" << boolean(evidence.pairValid)
      << ",\"action_profile_authenticated\":" << boolean(evidence.actionAuthenticated)
      << ",\"stable_exact_action_witness\":" << boolean(evidence.stableExact)
      << ",\"original_oracle_bytes_authenticated_only\":" << boolean(evidence.oracleBytesAuthenticated)
      << ",\"plan_digest_matched\":" << boolean(evidence.planDigestMatched)
      << ",\"blob_count\":" << evidence.blobCount << ",\"blob_bytes\":" << evidence.blobBytes << '}'
      << ",\"sha256\":{\"events\":" << quoted(renderTapeDigestHex(evidence.options.events))
      << ",\"identity\":" << quoted(renderTapeDigestHex(evidence.options.identity))
      << ",\"execution_topology\":" << quoted(renderTapeDigestHex(evidence.options.topology))
      << ",\"source\":" << quoted(renderTapeDigestHex(evidence.options.source))
      << ",\"output\":" << quoted(renderTapeDigestHex(evidence.options.output))
      << ",\"action_profile\":" << quoted(renderTapeDigestHex(evidence.options.action))
      << ",\"expected_plan\":" << quoted(renderTapeDigestHex(evidence.options.plan))
      << ",\"actual_plan\":" << quoted(renderTapeDigestHex(evidence.planDigest)) << '}'
      << ",\"action_witness\":{\"capture_token\":" << evidence.actionWitness.captureToken
      << ",\"capture_epoch\":" << evidence.actionWitness.captureEpoch
      << ",\"queue\":" << evidence.actionWitness.queue
      << ",\"construction_profile\":" << evidence.actionWitness.constructionProfile
      << ",\"arm_profile\":" << evidence.actionWitness.armProfile
      << ",\"freeze_profile\":" << evidence.actionWitness.freezeProfile
      << ",\"arm_revision\":" << evidence.actionWitness.armRevision
      << ",\"freeze_revision\":" << evidence.actionWitness.freezeRevision << '}'
      << ",\"plan_counts\":{\"events\":" << evidence.planEvents << ",\"records\":" << evidence.planRecords
      << ",\"sources\":" << evidence.planSources << '}'
      << ",\"metal_resource_device_created\":" << boolean(evidence.resourceDeviceCreated)
      << ",\"fixed_fixture_setup\":{\"cpu_ready_arena_replay_supported\":"
      << boolean(evidence.cpuReadyArenaSupported)
      << ",\"direct_chunk_slot_replay_supported\":" << boolean(evidence.directSlotSupported)
      << ",\"render_workers_disabled_verified\":" << boolean(evidence.workersDisabled)
      << ",\"replay_offload_absent_verified\":" << boolean(evidence.offloadAbsent)
      << ",\"exact_queue_action_profile_verified\":" << boolean(evidence.localExactProfile)
      << ",\"queue_construction_profile\":" << evidence.localConstructionProfile
      << ",\"queue_current_profile\":" << evidence.localCurrentProfile
      << ",\"queue_profile_revision\":" << evidence.localProfileRevision << '}'
      << ",\"fixture_teardown_complete\":" << boolean(evidence.teardownComplete)
      << ",\"authority_scope_valid\":" << boolean(evidence.authorityScopeValid)
      << ",\"input_admission\":";
  if (evidence.inputAdmission) {
    const auto& admission = *evidence.inputAdmission;
    std::cout << "{\"accepted\":" << boolean(admission.accepted())
        << ",\"reason\":" << quoted(std::string_view(
            runtime::generalCpuAdmissionReasonName(admission.reason())))
        << ",\"event_index\":" << admission.eventIndex()
        << ",\"chunk_index\":" << admission.chunkIndex()
        << ",\"record_index\":" << admission.recordIndex() << ",\"caps\":";
    printGeneralCpuInputCaps(admission.inputCaps());
    std::cout << ",\"counts\":";
    printGeneralCpuInputAdmissionCounts(admission.counts());
    std::cout << '}';
  } else std::cout << "null";
  std::cout << ",\"execution\":";
  if (evidence.result) printResult(*evidence.result); else std::cout << "null";
  std::cout << ",\"loaded_images\":";
  printCpuTapeLoadedImages();
  std::cout << "}\n";
}

}  // namespace controlled_inventory_cli

int runControlledCpuV4InventoryCli(int argc, char** argv) {
  controlled_inventory_cli::Evidence evidence;
  try {
    evidence.options = controlled_inventory_cli::arguments(argc, argv);
    controlled_inventory_cli::execute(evidence);
  } catch (const std::exception& error) {
    evidence.failure = evidence.result && !evidence.result->failure.empty()
        ? evidence.result->failure : error.what();
  } catch (...) {
    evidence.failure = "controlled-inventory-unclassified-exception";
  }
  controlled_inventory_cli::print(evidence);
  return evidence.failure.empty() ? 0 : evidence.result ? 4 : 3;
}
