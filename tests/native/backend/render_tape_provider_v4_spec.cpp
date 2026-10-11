#include "device_c_render_tape_provider.hpp"
#include "device_c_render_tape_runtime_objects.hpp"
#include "render_tape_cpu_destroy_fixture.hpp"
#include "device_c_render_tape_action_profile_component.hpp"
#include "device_c_render_tape_identity_transfer_v4.hpp"
#include "dxmt9/render_tape_attachment_history.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <new>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

// This executable alone intercepts synchronous C++ allocations to qualify
// eager factory unwind. Production/provider code contains no fault hook.
namespace prepared_v4_allocation_probe {
thread_local bool enabled = false;
thread_local bool failEnabled = false;
thread_local bool injected = false;
thread_local std::size_t count = 0u;
thread_local std::size_t failAfter = 0u;
} // namespace prepared_v4_allocation_probe

void* operator new(std::size_t bytes) {
  if (prepared_v4_allocation_probe::enabled) ++prepared_v4_allocation_probe::count;
  if (prepared_v4_allocation_probe::failEnabled) {
    if (prepared_v4_allocation_probe::failAfter == 0u) {
      prepared_v4_allocation_probe::failEnabled = false;
      prepared_v4_allocation_probe::injected = true;
      throw std::bad_alloc();
    }
    --prepared_v4_allocation_probe::failAfter;
  }
  if (auto* memory = std::malloc(bytes ? bytes : 1u)) return memory;
  throw std::bad_alloc();
}

void* operator new[](std::size_t bytes) { return ::operator new(bytes); }
void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete[](void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }
void operator delete[](void* memory, std::size_t) noexcept { std::free(memory); }

namespace {

using namespace dxmt9::d3d9;
using render_tape_runtime::GeneralPreflightPlan;
using Mode = RenderTapeSourceBoundaryMode;
using PublicationMode = RenderTapeExecutionV4PublicationMode;
std::uint64_t checks = 0u;

void check(bool value, std::string_view message) {
  ++checks;
  if (!value) throw std::runtime_error(std::string(message));
}

struct ScopedV4AllocationProbe {
  explicit ScopedV4AllocationProbe(bool inject, std::size_t ordinal = 0u) {
    prepared_v4_allocation_probe::count = 0u;
    prepared_v4_allocation_probe::injected = false;
    prepared_v4_allocation_probe::failAfter = ordinal;
    prepared_v4_allocation_probe::failEnabled = inject;
    prepared_v4_allocation_probe::enabled = true;
  }
  ~ScopedV4AllocationProbe() {
    prepared_v4_allocation_probe::enabled = false;
    prepared_v4_allocation_probe::failEnabled = false;
  }
  ScopedV4AllocationProbe(const ScopedV4AllocationProbe&) = delete;
  ScopedV4AllocationProbe& operator=(const ScopedV4AllocationProbe&) = delete;
};

struct Fixture {
  std::vector<std::byte> tape{}, identity{}, topology{};
  std::vector<std::vector<std::byte>> blobs{};
  std::vector<RenderTapeProviderBlob> references{};
  RenderTapeDigest planDigest{};
  PreparedRenderTape prepared{};
};

// These are declared captured rows admitted by the production wire validators,
// not evidence of this test performing GPU work. Every replay below uses a
// null device: only exact paired/pre-effect provider admission is qualified.
// The observer reducer tests below use authored borrows/callbacks, not runtime
// buffer contents, actual arm authority, or GPU replay evidence.
Fixture makeFixture(test::GeneralCpuResetFixture reset = test::GeneralCpuResetFixture::A,
    bool unsupportedLateSetup = false) {
  Fixture fixture;
  auto source = test::makeGeneralCpuDestroyFixture(false, false,
      test::GeneralCpuMutationFixture::None, reset);
  if (unsupportedLateSetup) {
    ImportedRenderTapeView imported;
    check(importPrevalidatedRenderTape(source.tape, imported), "late-setup source tape imports exactly");
    RenderTapeBuilder builder;
    bool inserted = false;
    for (std::size_t index = 0u; index < imported.events.size(); ++index) {
      const auto event = imported.event(index);
      const auto type = static_cast<RenderTapeEventType>(event.header.type);
      if (type == RenderTapeEventType::PresentComplete) {
        auto payload = std::vector<std::byte>(event.payload.begin(), event.payload.end());
        RenderTapePresentCompleteHeader completion{};
        std::memcpy(&completion, payload.data(), sizeof(completion));
        completion.presentOrdinal += 2u;
        std::memcpy(payload.data(), &completion, sizeof(completion));
        builder.appendRawEvent(type, payload);
      } else {
        builder.appendRawEvent(type, event.payload);
      }
      if (type == RenderTapeEventType::CommandChunk && !inserted) {
        // Authenticated General grammar supports this standalone seed, while
        // V4 initial setup deliberately rejects a late independent surface.
        const D9CWireObjectIdentity surface{D9C_CHUNK_HANDLE_KIND_SURFACE, 11u, 1020u};
        const RenderTapeSurfaceDescriptorV2 descriptor{
            .storage = static_cast<std::uint32_t>(RenderTapeSurfaceStorage::Standalone),
            .initialContentDisposition = static_cast<std::uint32_t>(RenderTapeInitialContentDisposition::CompleteSeed),
            .surface = {.format = 21u, .resourceType = 1u, .usage = 1u,
                .width = 4u, .height = 4u, .depth = 1u}};
        source.blobs.emplace_back(64u, std::byte{0xa7u});
        builder.appendObjectDefine(surface, static_cast<std::uint32_t>(RenderTapeDescriptorKind::Surface),
            std::as_bytes(std::span(&descriptor, 1u)), 0u, {}, 64u, 1u);
        builder.appendResourceMutation(surface, RenderTapeMutationKind::Upload, 0u, 0u, 64u,
            RenderTapeCaptureSession::sha256(source.blobs.back()));
        inserted = true;
      }
    }
    check(inserted, "unsupported late setup is inserted after a real original command boundary");
    source.tape = builder.seal();
  }
  fixture.tape = std::move(source.tape);
  fixture.blobs = std::move(source.blobs);
  for (const auto& blob : fixture.blobs)
    fixture.references.push_back({RenderTapeCaptureSession::sha256(blob), blob});
  GeneralPreflightPlan replay;
  check(render_tape_runtime::buildGeneralReplayPlan(fixture.tape,
      fixture.references, replay).complete(), "genuine indexed General Tape admits");

  RenderTapeIdentityV4View identity;
  identity.header.frameId = 9u;
  identity.header.captureToken = 100u;
  identity.header.captureEpoch = 101u;
  identity.header.queue = 102u;
  identity.header.initializerEpoch = 103u;
  identity.header.baselineCompletionEdge = 1u;
  identity.header.baselineKind = static_cast<std::uint32_t>(
      RenderTapeIdentityV3PrefixKind::EmptyBaseline);
  RenderTapeExecutionTopologyV1View topology;
  topology.header.captureToken = identity.header.captureToken;
  topology.header.captureEpoch = identity.header.captureEpoch;
  topology.header.queue = identity.header.queue;
  std::uint64_t edge = 2u, generation = 1u;
  for (const auto& event : replay.events) {
    if (event.type == RenderTapeEventType::PresentComplete) {
      const auto payload = replay.tape.event(event.eventIndex).payload;
      RenderTapePresentCompleteHeader completion{};
      check(payload.size() >= sizeof(completion), "Present completion is bounded");
      std::memcpy(&completion, payload.data(), sizeof(completion));
      identity.header.presentOrdinal = completion.presentOrdinal;
    }
    if (event.type != RenderTapeEventType::CommandChunk) continue;
    const auto ordinal = replay.tape.event(event.eventIndex).header.ordinal;
    const auto raw = 500u + ordinal;
    const auto& admitted = replay.commandChunks[event.index];
    ImportedChunkView chunk;
    check(importPrevalidatedCommandChunk(admitted.bytes, admitted.envelope, chunk) &&
        chunk.records.size() == 1u, "fixture raws contain one admitted record");
    const bool present = chunk.records[0].type == D9C_COMMAND_RECORD_PRESENT;
    const RenderTapeIdentityV4ReplaySourceIdentity projection{
        9u, 600u + ordinal, 600u + ordinal, raw};
    const auto begin = edge++, effect = edge++, terminal = edge++;
    identity.attempts.push_back({.eventOrdinal = ordinal, .rawOrdinal = raw,
        .attemptGeneration = generation++, .beginEdge = begin, .terminalEdge = terminal,
        .effectEdge = effect, .identity = projection,
        .receipt = {.identity = projection, .queueSequence = raw,
            .kind = static_cast<std::uint32_t>(RenderTapeIdentityV4ReceiptKind::Compatibility),
            .commandCount = 1u}, .recordCount = 1u,
        .outcome = static_cast<std::uint32_t>(RenderTapeIdentityV4AttemptOutcome::Committed),
        .stagedCommandCount = 1u});
    identity.settlements.push_back({ordinal, raw, edge++, 1u});
    const auto sourceIndex = static_cast<std::uint32_t>(identity.sources.size());
    const auto commandIndex = static_cast<std::uint32_t>(identity.commands.size());
    const auto publication = edge++, acceptance = edge++, submission = edge++, completion = edge++;
    identity.sources.push_back({.lifecycle = {.sourceOrdinal = sourceIndex + 1u,
        .seqId = sourceIndex + 1u, .writerCaptureId = sourceIndex + 1u,
        .writerGeneration = 2u, .storageGeneration = 3u,
        .publicationEdge = publication, .submissionEdge = submission,
        .completionEdge = completion, .completedSequence = sourceIndex + 1u,
        .cutEventOrdinal = ordinal, .commandCount = 1u,
        .cutKind = static_cast<std::uint32_t>(RenderTapeIdentityV3CutKind::AfterEvent)},
        .rawOrdinal = raw, .publicationBuildGeneration = generation - 1u,
        .publicationControlIndex = sourceIndex});
    identity.commands.push_back({.sourceIndex = sourceIndex,
        .kind = static_cast<std::uint32_t>(present ? RenderTapeIdentityV3CommandKind::Present
                                                : RenderTapeIdentityV3CommandKind::DrawRun),
        .drawCount = present ? 0u : 1u});
    identity.dispositions.push_back({.eventOrdinal = ordinal, .recordCount = 1u,
        .kind = static_cast<std::uint32_t>(RenderTapeIdentityV3DispositionKind::GpuCommand),
        .commandIndex = commandIndex, .subdrawCount = present ? 0u : 1u});
    topology.plans.push_back({.planId = sourceIndex + 11u, .acceptanceEdge = acceptance,
        .route = static_cast<std::uint32_t>(RenderTapeExecutionTopologyV1Route::NaturalOrder),
        .firstPlacement = commandIndex, .placementCount = 1u, .executedCount = 1u});
    topology.placements.push_back({.planIndex = sourceIndex, .commandIndex = commandIndex,
        .outcome = static_cast<std::uint32_t>(RenderTapeExecutionTopologyV1Outcome::ExecutedWithoutDag),
        .executionOrdinal = 0u});
  }
  RenderTapeIdentityV4Status identityStatus{};
  fixture.identity = buildRenderTapeIdentityV4(fixture.tape, replay.catalogue,
      identity, &identityStatus);
  check(identityStatus == RenderTapeIdentityV4Status::Valid && !fixture.identity.empty(),
      "declared captured V4 rows pass production provenance validation");
  RenderTapeExecutionTopologyV1Status topologyStatus{};
  fixture.topology = buildRenderTapeExecutionTopologyV1(fixture.tape, replay.catalogue,
      fixture.identity, topology, &topologyStatus);
  CaptureSourceReplayPlanV4 plan;
  check(topologyStatus == RenderTapeExecutionTopologyV1Status::Valid &&
      buildCaptureSourceReplayPlanV4(fixture.tape, replay.catalogue, fixture.identity,
          fixture.topology, plan) == CaptureSourceReplayPlanV4Status::Valid &&
      !plan.records().empty(), "full pair authenticates a nonempty canonical plan");
  fixture.planDigest = plan.digest();
  fixture.prepared = prepareRenderTapeIdentity(fixture.tape, fixture.references);
  check(fixture.prepared.valid(), "prepared provider retains the General owner");
  return fixture;
}

RenderTapePreparedExecutionOptions optionsFor(const Fixture& fixture) {
  RenderTapePreparedExecutionOptions options;
  options.sourceBoundaryMode = Mode::CaptureSourceV4ExactDiagnostic;
  options.sourceBoundaryIdentity = fixture.identity;
  options.sourceExecutionTopology = fixture.topology;
  options.expectedSourcePlanDigest = fixture.planDigest;
  options.hasExpectedSourcePlanDigest = true;
  return options;
}

RenderTapePreparedV4PreparationOptions immutableOptionsFor(const Fixture& fixture) {
  RenderTapePreparedV4PreparationOptions options;
  options.sourceBoundaryIdentity = fixture.identity;
  options.sourceExecutionTopology = fixture.topology;
  options.expectedSourcePlanDigest = fixture.planDigest;
  return options;
}

void noEffects(const FrameTapeReplayResult& result, bool requested = true) {
  const auto& evidence = result.exactDiagnosticV4.execution;
  check(evidence.requested == requested && evidence.qualifierOnly &&
      !evidence.policyQualified && !evidence.queryReadinessQualified &&
      !evidence.pixelsQualified && !evidence.timingEligible,
      "diagnostic admission cannot promote independent qualification gates");
  check(!evidence.armed && !evidence.complete && !evidence.actualAttemptsObserved &&
      !evidence.actualLifecycleComplete && !evidence.actualTopologyComplete &&
      !evidence.exactSourceDiagnosticComplete && !evidence.semanticMappingComplete &&
      !evidence.semanticMapping && !evidence.canonicalTraversalComplete &&
      !evidence.rawAckCoverageComplete && !evidence.localArm.queue &&
      !evidence.localFinish.queue && evidence.replayedEvents == 0u &&
      evidence.dispatchedRecords == 0u && evidence.acknowledgedRaws == 0u &&
      evidence.actualIdentity.attempts.empty() && evidence.actualTopology.plans.empty() &&
      evidence.sources.empty() && evidence.initializers.empty() && evidence.coordinators.empty(),
      "null-device/pre-effect checks have no actual replay or GPU provenance");
  check(!result.prepared.resetPerformed && !result.prepared.resetVerified &&
      result.conservation.objectsCreated == 0u && result.conservation.objectsReleased == 0u &&
      !result.sourceReplay.requested && !result.sourceReplayV3.requested &&
      !result.queryReplay.gpuResultOracle && !result.validity.outputReadback,
      "independent policy/query/pixel/reset sinks remain unqualified");
  const auto& timing = result.prepared.timing;
  check(timing.v4ReplayAdmissionNanos == 0u &&
      timing.v4PreArmCheckpointNanos == 0u && timing.v4FinishNanos == 0u,
      "null-device/pre-effect rejection cannot record unentered V4 replay phases");
}

void admission() {
  const auto fixture = makeFixture();
  check(optionsFor(fixture).publicationModeV4 == PublicationMode::ExactSourceDiagnostic,
      "existing private V4 caller defaults to unchanged exact-source qualification");
  for (const auto mode : {PublicationMode::ExactSourceDiagnostic, PublicationMode::SemanticMapping}) {
    for (const auto route : {RenderTapePreparedRoute::Compatibility,
                            RenderTapePreparedRoute::ProductionSlot}) {
      auto options = optionsFor(fixture);
      options.route = route;
      options.publicationModeV4 = mode;
      const auto result = replayPreparedRenderTapeIdentity(nullptr, fixture.prepared, options);
      check(result.status == FrameTapeReplayStatus::ObjectCreationFailed &&
          result.exactDiagnosticV4.execution.validated &&
          result.exactDiagnosticV4.execution.admissionCount == 1u &&
          !result.exactDiagnosticV4.execution.preparedAdmissionConsumed &&
          result.exactDiagnosticV4.execution.planDigest == fixture.planDigest &&
          result.exactDiagnosticV4.execution.publicationMode == mode &&
          result.prepared.requestedRoute == route &&
          std::string_view(result.exactDiagnosticV4.failure) == "v4-provider-device-missing",
          "both private publication modes retain requested route and paired admission before device use");
      noEffects(result);
      const auto repeated = replayPreparedRenderTapeIdentity(nullptr, fixture.prepared, options);
      check(repeated.status == result.status &&
          repeated.exactDiagnosticV4.execution.admissionCount == 1u &&
          !repeated.exactDiagnosticV4.execution.preparedAdmissionConsumed &&
          repeated.exactDiagnosticV4.execution.planDigest == fixture.planDigest &&
          fixture.prepared.prepareCount() == 1u &&
          std::string_view(repeated.exactDiagnosticV4.failure) == "v4-provider-device-missing",
          "same immutable prepared input creates a fresh single admission per null-device attempt, never cached executor state");
      noEffects(repeated);
    }
  }

  for (std::uint32_t fault = 0u; fault != 7u; ++fault) {
    auto options = optionsFor(fixture);
    auto identity = fixture.identity;
    auto topology = fixture.topology;
    if (fault == 0u) options.sourceBoundaryIdentity = {};
    if (fault == 1u) options.sourceExecutionTopology = {};
    if (fault == 2u) options.hasExpectedSourcePlanDigest = false;
    if (fault == 3u) options.expectedSourcePlanDigest = {};
    if (fault == 4u) options.expectedSourcePlanDigest[0] ^= std::byte{1u};
    if (fault == 5u) {
      identity[0] ^= std::byte{1u};
      options.sourceBoundaryIdentity = identity;
    }
    if (fault == 6u) {
      topology[0] ^= std::byte{1u};
      options.sourceExecutionTopology = topology;
    }
    const auto result = replayPreparedRenderTapeIdentity(nullptr, fixture.prepared, options);
    check(result.status == FrameTapeReplayStatus::InvalidTape &&
        !result.exactDiagnosticV4.execution.validated &&
        result.exactDiagnosticV4.execution.admissionCount == 0u &&
        !result.exactDiagnosticV4.execution.preparedAdmissionConsumed &&
        std::string_view(result.exactDiagnosticV4.failure) != "none",
        "missing/corrupt pair or missing/mismatched digest rejects before effects");
    noEffects(result);
  }
  const auto alternate = makeFixture(test::GeneralCpuResetFixture::B);
  const auto wrongOwner = replayPreparedRenderTapeIdentity(nullptr, fixture.prepared,
      optionsFor(alternate));
  check(wrongOwner.status == FrameTapeReplayStatus::InvalidTape &&
      !wrongOwner.exactDiagnosticV4.execution.validated &&
      wrongOwner.exactDiagnosticV4.execution.admissionCount == 0u &&
      !wrongOwner.exactDiagnosticV4.execution.preparedAdmissionConsumed,
      "a valid pair for another exact Tape cannot authorize the retained owner");
  noEffects(wrongOwner);
}

void immutablePreparedInputLifetime() {
  for (const auto mode : {PublicationMode::ExactSourceDiagnostic, PublicationMode::SemanticMapping}) {
    PreparedRenderTape owner;
    RenderTapeDigest expectedDigest{};
    {
      auto input = makeFixture();
      auto options = immutableOptionsFor(input);
      options.publicationMode = mode;
      expectedDigest = input.planDigest;
      owner = prepareRenderTapeIdentityV4(input.tape, input.references, options);
      check(owner.valid() && owner.prepareCount() == 1u &&
          owner.evidence().immutableV4PreparationOwned && owner.evidence().v4PreparationCount == 1u &&
          !owner.evidence().reusedImmutablePreparation,
          "eager factory publishes one authenticated immutable V4 owner, not execution or reused-runtime authority");
      std::fill(input.tape.begin(), input.tape.end(), std::byte{0xffu});
      std::fill(input.identity.begin(), input.identity.end(), std::byte{0xffu});
      std::fill(input.topology.begin(), input.topology.end(), std::byte{0xffu});
      for (auto& blob : input.blobs) std::fill(blob.begin(), blob.end(), std::byte{0xffu});
      options.expectedSourcePlanDigest = {};
      options.publicationMode = static_cast<PublicationMode>(255u);
      options.producerContract = static_cast<RenderTapeExecutionV4ProducerContract>(255u);
      const auto result = replayPreparedRenderTapeIdentityV4(nullptr, owner,
          RenderTapePreparedRoute::Compatibility);
      check(result.exactDiagnosticV4.execution.validated &&
          result.exactDiagnosticV4.execution.planDigest == expectedDigest &&
          result.exactDiagnosticV4.execution.publicationMode == mode &&
          result.exactDiagnosticV4.execution.producerContract == RenderTapeExecutionV4ProducerContract::None,
          "caller mutation of tape/blob/pair/options cannot alter already-authenticated owned preparation");
      noEffects(result);
    }
    PreparedRenderTape moved(std::move(owner));
    check(moved.valid() && !owner.valid() && owner.prepareCount() == 0u &&
        moved.evidence().immutableV4PreparationOwned && moved.evidence().v4PreparationCount == 1u,
        "public owner move preserves stable PIMPL storage after all loader inputs are destroyed");
    PreparedRenderTape assigned;
    assigned = std::move(moved);
    check(assigned.valid() && !moved.valid() && moved.prepareCount() == 0u,
        "move assignment transfers the sole immutable owner without reviving moved-from authority");
    for (const auto route : {RenderTapePreparedRoute::Compatibility, RenderTapePreparedRoute::ProductionSlot}) {
      for (std::uint32_t invocation = 0u; invocation < 2u; ++invocation) {
        const auto result = replayPreparedRenderTapeIdentityV4(nullptr, assigned, route);
        check(result.status == FrameTapeReplayStatus::ObjectCreationFailed &&
            std::string_view(result.exactDiagnosticV4.failure) == "v4-provider-device-missing" &&
            result.exactDiagnosticV4.execution.validated &&
            result.exactDiagnosticV4.execution.admissionCount == 1u &&
            !result.exactDiagnosticV4.execution.preparedAdmissionConsumed &&
            result.exactDiagnosticV4.execution.planDigest == expectedDigest &&
            result.exactDiagnosticV4.execution.publicationMode == mode &&
            result.exactDiagnosticV4.execution.producerContract == RenderTapeExecutionV4ProducerContract::None &&
            result.prepared.requestedRoute == route && result.prepared.reusedImmutablePreparation &&
            result.prepared.immutableV4PreparationOwned && result.prepared.v4PreparationCount == 1u &&
            !assigned.evidence().reusedImmutablePreparation,
            "named replay uses frozen policy and fresh per-invocation admission without repeating immutable preparation");
        noEffects(result);
      }
    }
    for (const auto* invalid : {&owner, &moved}) {
      const auto result = replayPreparedRenderTapeIdentityV4(nullptr, *invalid,
          RenderTapePreparedRoute::Compatibility);
      check(!result.complete() && !result.exactDiagnosticV4.execution.validated &&
          result.exactDiagnosticV4.execution.admissionCount == 0u &&
          !result.prepared.immutableV4PreparationOwned && !result.prepared.v4PreparationCount &&
          !result.prepared.reusedImmutablePreparation,
          "moved-from public input owner cannot authorize another V4 invocation");
      noEffects(result, result.exactDiagnosticV4.execution.requested);
    }
  }
  const auto ordinary = makeFixture();
  const auto result = replayPreparedRenderTapeIdentityV4(nullptr, ordinary.prepared,
      RenderTapePreparedRoute::Compatibility);
  check(!result.complete() && !result.exactDiagnosticV4.execution.validated &&
      !result.prepared.immutableV4PreparationOwned && !result.prepared.v4PreparationCount &&
      !result.prepared.reusedImmutablePreparation,
      "ordinary General preparation is not authority for the explicitly immutable V4 entrypoint");
  noEffects(result, result.exactDiagnosticV4.execution.requested);
}

void immutablePreparedFactoryFailures() {
  const auto fixture = makeFixture();
  for (std::uint32_t fault = 0u; fault < 9u; ++fault) {
    auto options = immutableOptionsFor(fixture);
    auto identity = fixture.identity;
    auto topology = fixture.topology;
    auto tape = std::span<const std::byte>(fixture.tape);
    if (fault == 0u) options.sourceBoundaryIdentity = {};
    if (fault == 1u) options.sourceExecutionTopology = {};
    if (fault == 2u) options.expectedSourcePlanDigest = {};
    if (fault == 3u) options.expectedSourcePlanDigest[0] ^= std::byte{1u};
    if (fault == 4u) {
      identity[0] ^= std::byte{1u};
      options.sourceBoundaryIdentity = identity;
    }
    if (fault == 5u) {
      topology[0] ^= std::byte{1u};
      options.sourceExecutionTopology = topology;
    }
    if (fault == 6u) options.publicationMode = static_cast<PublicationMode>(255u);
    if (fault == 7u)
      options.producerContract = static_cast<RenderTapeExecutionV4ProducerContract>(255u);
    if (fault == 8u) tape = tape.first(tape.size() - 1u);
    const auto rejected = prepareRenderTapeIdentityV4(tape, fixture.references, options);
    check(!rejected.valid() && rejected.prepareCount() == 0u &&
        !rejected.evidence().immutableV4PreparationOwned && !rejected.evidence().v4PreparationCount,
        "incomplete/malformed pair, digest, policy or tape publishes no partial immutable V4 owner");
    const auto result = replayPreparedRenderTapeIdentityV4(nullptr, rejected,
        RenderTapePreparedRoute::Compatibility);
    check(!result.complete() && !result.exactDiagnosticV4.execution.validated &&
        !result.exactDiagnosticV4.execution.admissionCount && !result.prepared.reusedImmutablePreparation,
        "factory rejection cannot be converted into invocation authority by calling named replay");
    noEffects(result, result.exactDiagnosticV4.execution.requested);
  }
  const auto alternate = makeFixture(test::GeneralCpuResetFixture::B);
  const auto wrongOwner = prepareRenderTapeIdentityV4(fixture.tape,
      fixture.references, immutableOptionsFor(alternate));
  check(!wrongOwner.valid() && !wrongOwner.evidence().immutableV4PreparationOwned &&
      !wrongOwner.evidence().v4PreparationCount,
      "valid V4 pair for another exact tape cannot authenticate the eager immutable owner");
  const auto lateSetup = makeFixture(test::GeneralCpuResetFixture::A, true);
  const auto legacySetup = replayPreparedRenderTapeIdentity(nullptr, lateSetup.prepared,
      optionsFor(lateSetup));
  check(legacySetup.exactDiagnosticV4.execution.validated &&
      !legacySetup.exactDiagnosticV4.initialSetupAdmitted &&
      std::string_view(legacySetup.exactDiagnosticV4.failure) == "v4-initial-setup-admission-failed",
      "setup negative reaches authenticated executor admission before its deliberately unsupported late seed closure");
  noEffects(legacySetup);
  const auto rejectedSetup = prepareRenderTapeIdentityV4(lateSetup.tape, lateSetup.references,
      immutableOptionsFor(lateSetup));
  check(!rejectedSetup.valid() && !rejectedSetup.evidence().immutableV4PreparationOwned &&
      !rejectedSetup.evidence().v4PreparationCount,
      "authenticated unsupported initial setup publishes no partial eager immutable owner");

  const auto options = immutableOptionsFor(fixture);
  PreparedRenderTape successful;
  {
    ScopedV4AllocationProbe probe(false);
    successful = prepareRenderTapeIdentityV4(fixture.tape, fixture.references, options);
  }
  const auto allocations = prepared_v4_allocation_probe::count;
  constexpr std::size_t allocationSweepLimit = 4096u;
  check(successful.valid() && allocations != 0u && allocations <= allocationSweepLimit,
      "eager factory allocation interception observes a nonzero bounded synchronous allocation path");
  for (std::size_t allocation = 0u; allocation < allocations; ++allocation) {
    PreparedRenderTape rejected;
    {
      ScopedV4AllocationProbe probe(true, allocation);
      rejected = prepareRenderTapeIdentityV4(fixture.tape, fixture.references, options);
    }
    check(prepared_v4_allocation_probe::injected && !rejected.valid() && rejected.prepareCount() == 0u &&
        !rejected.evidence().immutableV4PreparationOwned && !rejected.evidence().v4PreparationCount,
        "each actually intercepted bad_alloc leaves no usable or partially authenticated immutable factory result");
    const auto result = replayPreparedRenderTapeIdentityV4(nullptr, rejected,
        RenderTapePreparedRoute::Compatibility);
    check(!result.exactDiagnosticV4.execution.validated &&
        !result.exactDiagnosticV4.execution.admissionCount && !result.prepared.reusedImmutablePreparation,
        "allocation failure grants no later per-invocation authority");
    noEffects(result, result.exactDiagnosticV4.execution.requested);
  }
  FrameTapeReplayResult baselineReplay;
  {
    ScopedV4AllocationProbe probe(false);
    baselineReplay = replayPreparedRenderTapeIdentityV4(nullptr, successful,
        RenderTapePreparedRoute::Compatibility);
  }
  const auto replayAllocations = prepared_v4_allocation_probe::count;
  check(baselineReplay.exactDiagnosticV4.execution.validated && replayAllocations != 0u &&
      replayAllocations <= allocationSweepLimit,
      "named null-device replay probe observes bounded result-copy/fresh-initializer allocations before any effect");
  for (std::size_t allocation = 0u; allocation < replayAllocations; ++allocation) {
    FrameTapeReplayResult rejected;
    {
      ScopedV4AllocationProbe probe(true, allocation);
      rejected = replayPreparedRenderTapeIdentityV4(nullptr, successful,
          RenderTapePreparedRoute::Compatibility);
    }
    check(prepared_v4_allocation_probe::injected &&
        rejected.status == FrameTapeReplayStatus::ObjectCreationFailed &&
        (std::string_view(rejected.exactDiagnosticV4.failure) == "v4-replay-result-allocation" ||
         std::string_view(rejected.exactDiagnosticV4.failure) == "v4-admission-allocation") &&
        !rejected.exactDiagnosticV4.execution.validated && !rejected.exactDiagnosticV4.execution.admissionCount &&
        !rejected.prepared.reusedImmutablePreparation &&
        successful.valid() && successful.evidence().immutableV4PreparationOwned &&
        successful.evidence().v4PreparationCount == 1u,
        "each intercepted pre-effect replay allocation failure rejects local authority without terminating or altering immutable owner");
    noEffects(rejected, rejected.exactDiagnosticV4.execution.requested);
    const auto recovered = replayPreparedRenderTapeIdentityV4(nullptr, successful,
        RenderTapePreparedRoute::Compatibility);
    check(recovered.exactDiagnosticV4.execution.validated &&
        recovered.exactDiagnosticV4.execution.admissionCount == 1u &&
        !recovered.exactDiagnosticV4.execution.preparedAdmissionConsumed &&
        recovered.prepared.reusedImmutablePreparation && recovered.prepared.v4PreparationCount == 1u,
        "invocation after allocation failure initializes fresh local state from the unchanged single preparation");
    noEffects(recovered);
  }
}

void modes() {
  const auto fixture = makeFixture();
  const std::array<RenderTapeBatchCut, 1u> cuts{{{9u, 0u}}};
  const RenderTapeTextureInputAuditRequest input;
  for (const auto mode : {PublicationMode::ExactSourceDiagnostic, PublicationMode::SemanticMapping}) {
    for (std::uint32_t fault = 0u; fault != 10u; ++fault) {
      auto options = optionsFor(fixture);
      options.publicationModeV4 = mode;
      if (fault == 0u) options.reset = RenderTapePreparedReset::WarmDevice;
      if (fault == 1u) options.auditInitialSeeds = true;
      if (fault == 2u) options.auditFreshSeedDrains = true;
      if (fault == 3u) options.auditTexturePrestage = true;
      if (fault == 4u) options.auditSourceBoundaries = true;
      if (fault == 5u) options.auditBatchCuts = cuts;
      if (fault == 6u) options.auditTextureInput = &input;
      if (fault == 7u) options.route = static_cast<RenderTapePreparedRoute>(255u);
      if (fault == 8u) options.reset = static_cast<RenderTapePreparedReset>(255u);
      if (fault == 9u) options.publicationModeV4 = static_cast<PublicationMode>(255u);
      const auto result = replayPreparedRenderTapeIdentity(nullptr, fixture.prepared, options);
      check(result.status == FrameTapeReplayStatus::UnsupportedGrammar &&
          !result.exactDiagnosticV4.execution.validated &&
          std::string_view(result.exactDiagnosticV4.failure) == "v4-provider-options-unsupported",
          "V4 diagnostic is fresh-only, valid-route and exclusive with every competing audit");
      check(result.prepared.timing.v4PlanAdmissionNanos == 0u,
          "unsupported options reject before the V4 plan admission scope");
      noEffects(result);
    }
  }
  for (const auto mode : {Mode::Chunk, Mode::CaptureSource, Mode::CaptureSourceV3}) {
    auto options = optionsFor(fixture);
    options.sourceBoundaryMode = mode;
    const auto result = replayPreparedRenderTapeIdentity(nullptr, fixture.prepared, options);
    check(result.status == FrameTapeReplayStatus::UnsupportedGrammar &&
        !result.exactDiagnosticV4.execution.validated,
        "V4 topology input cannot open ordinary or existing source-policy modes");
    // Those modes keep their own requested flags; no V4 qualification is made.
    check(!result.exactDiagnosticV4.execution.requested &&
        !result.exactDiagnosticV4.execution.complete &&
        !result.exactDiagnosticV4.execution.policyQualified &&
        !result.exactDiagnosticV4.execution.timingEligible &&
        !result.prepared.resetPerformed && result.conservation.objectsCreated == 0u,
        "out-of-mode companion rejection has no V4 or reset/resource effects");
  }
  const PreparedRenderTape empty;
  for (const auto mode : {Mode::Chunk, Mode::CaptureSource, Mode::CaptureSourceV3}) {
    RenderTapePreparedExecutionOptions options;
    options.sourceBoundaryMode = mode;
    options.producerContractV4 = RenderTapeExecutionV4ProducerContract::PePresentReservationV1;
    const auto result = replayPreparedRenderTapeIdentity(nullptr, fixture.prepared, options);
    check(result.status == FrameTapeReplayStatus::UnsupportedGrammar &&
        !result.prepared.resetPerformed && result.conservation.objectsCreated == 0u,
        "capture-Present producer contract cannot escape the private V4 diagnostic");
    options.producerContractV4 = RenderTapeExecutionV4ProducerContract::None;
    options.publicationModeV4 = PublicationMode::SemanticMapping;
    const auto semantic = replayPreparedRenderTapeIdentity(nullptr, fixture.prepared, options);
    check(semantic.status == FrameTapeReplayStatus::UnsupportedGrammar &&
        !semantic.exactDiagnosticV4.execution.requested &&
        !semantic.exactDiagnosticV4.execution.validated && !semantic.prepared.resetPerformed &&
        semantic.conservation.objectsCreated == 0u,
        "semantic provenance mode cannot open ordinary or existing source-policy paths");
  }
  const auto unprepared = replayPreparedRenderTapeIdentity(nullptr, empty, optionsFor(fixture));
  check(unprepared.status == FrameTapeReplayStatus::InvalidTape &&
      std::string_view(unprepared.exactDiagnosticV4.failure) == "v4-provider-preflight-incomplete",
      "missing immutable prepared owner stays fail-closed");
  noEffects(unprepared);
}

void observerAdmission() {
  const auto fixture = makeFixture();
  std::size_t calls = 0u;
  RenderTapePreparedObserverV4 observer{
      .context = &calls,
      .buffer = [](void* context, D9CDevice*,
          const RenderTapePreparedBufferEventV4&) noexcept {
        ++*static_cast<std::size_t*>(context);
        return true;
      },
      .beforeRecord = [](void* context, D9CDevice*, std::uint64_t,
          std::uint32_t, std::uint32_t,
          std::span<const RenderTapePreparedHandleAssociationV4>) noexcept {
        ++*static_cast<std::size_t*>(context);
        return true;
      },
      .armed = [](void* context, D9CDevice*, std::int32_t,
          const D9CRenderTapeIdentityCaptureV4ArmResult&,
          const std::shared_ptr<RenderTapeRuntimeIdentityV4>&) noexcept {
        ++*static_cast<std::size_t*>(context);
        return true;
      },
      .closed = [](void* context, D9CDevice*) noexcept {
        ++*static_cast<std::size_t*>(context);
        return true;
      },
  };
  for (const auto mode : {Mode::Chunk, Mode::CaptureSource, Mode::CaptureSourceV3}) {
    RenderTapePreparedExecutionOptions options;
    options.sourceBoundaryMode = mode;
    options.observerV4 = &observer;
    const auto result = replayPreparedRenderTapeIdentity(nullptr, fixture.prepared, options);
    check(result.status == FrameTapeReplayStatus::UnsupportedGrammar &&
        result.preparedObserverV4.requested && !result.preparedObserverV4.admitted &&
        !result.preparedObserverV4.complete && !result.preparedObserverV4.closed &&
        std::string_view(result.preparedObserverV4.failure) == "v4-observer-options-unsupported" &&
        calls == 0u,
        "prepared observer cannot open non-V4 modes or perform any callback effect");
    check(!result.prepared.resetPerformed && result.conservation.objectsCreated == 0u &&
        !result.exactDiagnosticV4.execution.complete &&
        !result.exactDiagnosticV4.execution.policyQualified,
        "observer mode rejection leaves reset, renderer and qualification untouched");
  }
  for (const auto mode : {PublicationMode::ExactSourceDiagnostic, PublicationMode::SemanticMapping}) {
    auto options = optionsFor(fixture);
    options.publicationModeV4 = mode;
    options.observerV4 = &observer;
    const auto result = replayPreparedRenderTapeIdentity(nullptr, fixture.prepared, options);
    check(result.status == FrameTapeReplayStatus::ObjectCreationFailed &&
        result.exactDiagnosticV4.execution.validated &&
        result.preparedObserverV4.requested && !result.preparedObserverV4.complete &&
        !result.preparedObserverV4.closed && calls == 0u &&
        std::string_view(result.preparedObserverV4.failure) == "v4-observer-device-missing",
        "private V4 observer passes unchanged paired admission but needs an actual device");
    noEffects(result);
  }
  const PreparedRenderTape empty;
  auto options = optionsFor(fixture);
  options.observerV4 = &observer;
  const auto result = replayPreparedRenderTapeIdentity(nullptr, empty, options);
  check(result.preparedObserverV4.requested && !result.preparedObserverV4.admitted &&
      !result.preparedObserverV4.complete && !result.preparedObserverV4.closed && calls == 0u &&
      std::string_view(result.preparedObserverV4.failure) == "v4-observer-preflight-incomplete",
      "missing prepared input cannot invoke even observer closure");
}

void observerForwarding() {
  struct Context {
    std::array<std::uint32_t, 32u> order{};
    std::size_t count = 0u;
    bool inputsMatched = true;
    bool acceptObserver = false;
    bool acceptOriginal = true;
    const RenderTapePreparedHandleAssociationV4* associations = nullptr;
    const RenderTapeObjectDefineHeader* definition = nullptr;
    const RenderTapeResourceMutationHeader* mutation = nullptr;
    const std::byte* descriptor = nullptr;
    D9CBuffer* buffer = nullptr;
    void append(std::uint32_t value) noexcept { order[count++] = value; }
  } context;
  int authoredWrapper = 0;
  RenderTapeObjectDefineHeader definition{};
  RenderTapeResourceMutationHeader mutation{};
  const std::array<std::byte, 1u> descriptor{std::byte{3u}};
  const std::array<RenderTapePreparedHandleAssociationV4, 1u> associations{{
      {2u, {D9C_CHUNK_HANDLE_KIND_BUFFER, 7u, 41u}, &authoredWrapper}}};
  context.associations = associations.data();
  context.definition = &definition;
  context.mutation = &mutation;
  context.descriptor = descriptor.data();
  context.buffer = reinterpret_cast<D9CBuffer*>(&authoredWrapper);
  RenderTapePreparedObserverV4 observer{
      .context = &context,
      .buffer = [](void* opaque, D9CDevice* device,
          const RenderTapePreparedBufferEventV4& event) noexcept {
        auto& context = *static_cast<Context*>(opaque);
        context.append(7u);
        context.inputsMatched &= !device && event.eventIndex == 5u && event.eventOrdinal == 53u &&
            event.definition == context.definition && event.descriptor.data() == context.descriptor &&
            event.descriptor.size() == 1u && event.mutation == context.mutation &&
            event.buffer == context.buffer;
        return context.acceptObserver;
      },
      .beforeRecord = [](void* opaque, D9CDevice* device, std::uint64_t event,
          std::uint32_t index, std::uint32_t type,
          std::span<const RenderTapePreparedHandleAssociationV4> handles) noexcept {
        auto& context = *static_cast<Context*>(opaque);
        context.append(1u);
        context.inputsMatched &= !device && event == 53u && index == 4u && type == 8u &&
            handles.data() == context.associations && handles.size() == 1u &&
            handles[0].handleIndex == 2u && handles[0].identity.objectId == 41u &&
            handles[0].resolvedWrapper == context.associations[0].resolvedWrapper;
        return context.acceptObserver;
      },
      .armed = [](void* opaque, D9CDevice* device, std::int32_t result,
          const D9CRenderTapeIdentityCaptureV4ArmResult& arm,
          const std::shared_ptr<RenderTapeRuntimeIdentityV4>& root) noexcept {
        auto& context = *static_cast<Context*>(opaque);
        context.append(8u);
        context.inputsMatched &= !device && result == 19 && arm.captureToken == 17u &&
            arm.captureEpoch == 23u && arm.queue == 29u && !root;
        return context.acceptObserver;
      },
      .closed = [](void* opaque, D9CDevice* device) noexcept {
        auto& context = *static_cast<Context*>(opaque);
        context.append(9u);
        context.inputsMatched &= !device;
        return true;
      },
  };
  ProviderReplayObservationV4 original{
      .captureToken = 17u, .captureEpoch = 23u, .queue = 29u, .eventOrdinal = 53u,
      .context = &context,
      .rawStarted = [](void* opaque, std::uint64_t event, std::uint64_t raw,
          std::uint32_t count) noexcept {
        auto& context = *static_cast<Context*>(opaque);
        context.append(3u);
        context.inputsMatched &= event == 53u && raw == 61u && count == 2u;
        return context.acceptOriginal;
      },
      .beforeRecord = [](void* opaque, std::uint32_t index, std::uint32_t type) noexcept {
        auto& context = *static_cast<Context*>(opaque);
        context.append(2u);
        context.inputsMatched &= index == 4u && type == 8u;
        return context.acceptOriginal;
      },
      .beforeDispatch = [](void* opaque, std::uint32_t index, std::uint32_t type) noexcept {
        auto& context = *static_cast<Context*>(opaque);
        context.append(4u);
        context.inputsMatched &= index == 4u && type == 8u;
        return context.acceptOriginal;
      },
      .recordDispatched = [](void* opaque, std::uint32_t index, std::uint32_t type,
          std::int32_t result) noexcept {
        auto& context = *static_cast<Context*>(opaque);
        context.append(5u);
        context.inputsMatched &= index == 4u && type == 8u && result == -9;
        return context.acceptOriginal;
      },
      .rawSettled = [](void* opaque, std::uint64_t raw, std::uint32_t count,
          bool succeeded) noexcept {
        auto& context = *static_cast<Context*>(opaque);
        context.append(6u);
        context.inputsMatched &= raw == 61u && count == 2u && !succeeded;
        return context.acceptOriginal;
      },
  };
  RenderTapePreparedObserverV4Evidence evidence;
  render_tape_runtime::PreparedObserverV4Session session(&observer, evidence, nullptr);
  const auto forwarded = session.observation(original, associations);
  check(forwarded.captureToken == 17u && forwarded.captureEpoch == 23u &&
      forwarded.queue == 29u && forwarded.eventOrdinal == 53u,
      "production forwarding preserves every original capture credential");
  check(forwarded.rawStarted(forwarded.context, 53u, 61u, 2u) &&
      forwarded.beforeRecord(forwarded.context, 4u, 8u) &&
      forwarded.beforeDispatch(forwarded.context, 4u, 8u) &&
      forwarded.recordDispatched(forwarded.context, 4u, 8u, -9) &&
      forwarded.rawSettled(forwarded.context, 61u, 2u, false),
      "observer rejection never changes any original observation result");
  check(context.count == 6u && context.order[0] == 3u && context.order[1] == 1u &&
      context.order[2] == 2u && context.order[3] == 4u && context.order[4] == 5u &&
      context.order[5] == 6u && context.inputsMatched,
      "prepared beforeRecord runs first and all original callbacks still run exactly once");
  const RenderTapePreparedBufferEventV4 event{
      .phase = RenderTapePreparedBufferPhaseV4::HiddenDefined,
      .eventIndex = 5u, .eventOrdinal = 53u, .definition = &definition,
      .descriptor = descriptor, .mutation = &mutation, .buffer = context.buffer};
  const D9CRenderTapeIdentityCaptureV4ArmResult arm{
      .captureToken = 17u, .captureEpoch = 23u, .queue = 29u};
  const std::shared_ptr<RenderTapeRuntimeIdentityV4> root;
  session.buffer(event);
  session.armed(19, arm, root);
  check(forwarded.beforeRecord(forwarded.context, 4u, 8u) && context.count == 7u &&
      context.order[6] == 2u && !session.enabled(),
      "rejection disables later buffer/arm/record data, without disabling original records");
  session.closed(true);
  session.closed(true);
  check(context.count == 8u && context.order[7] == 9u && evidence.closed &&
      evidence.closedCallbacks == 1u && evidence.beforeRecordCallbacks == 1u &&
      evidence.bufferCallbacks == 0u && evidence.armedCallbacks == 0u &&
      evidence.requested && evidence.admitted && !evidence.complete &&
      std::string_view(evidence.failure) == "v4-observer-record-rejected",
      "closure is exactly once after rejection and separate diagnostic failure remains authoritative");

  context.count = 0u;
  context.acceptObserver = true;
  context.acceptOriginal = false;
  RenderTapePreparedObserverV4Evidence acceptedEvidence;
  render_tape_runtime::PreparedObserverV4Session accepted(&observer, acceptedEvidence, nullptr);
  const auto originalFailure = accepted.observation(original, associations);
  check(!originalFailure.rawStarted(originalFailure.context, 53u, 61u, 2u) &&
      !originalFailure.beforeRecord(originalFailure.context, 4u, 8u) &&
      !originalFailure.beforeDispatch(originalFailure.context, 4u, 8u) &&
      !originalFailure.recordDispatched(originalFailure.context, 4u, 8u, -9) &&
      !originalFailure.rawSettled(originalFailure.context, 61u, 2u, false) && accepted.enabled(),
      "prepared observer cannot turn original execution rejection into acceptance");
  for (std::uint32_t phase = 0u; phase < acceptedEvidence.bufferPhases.size(); ++phase) {
    auto authored = event;
    authored.phase = static_cast<RenderTapePreparedBufferPhaseV4>(phase);
    accepted.buffer(authored);
  }
  accepted.armed(19, arm, root);
  accepted.closed(true);
  check(context.inputsMatched && acceptedEvidence.complete && acceptedEvidence.closed &&
      acceptedEvidence.bufferCallbacks == 7u && acceptedEvidence.armedCallbacks == 1u &&
      acceptedEvidence.closedCallbacks == 1u &&
      std::all_of(acceptedEvidence.bufferPhases.begin(), acceptedEvidence.bufferPhases.end(),
          [](std::size_t count) { return count == 1u; }),
      "all typed phases and receipt/definition/descriptor/mutation/wrapper borrows pass unchanged");
  RenderTapePreparedObserverV4Evidence absentEvidence;
  render_tape_runtime::PreparedObserverV4Session absent(nullptr, absentEvidence, nullptr);
  const auto unchanged = absent.observation(original, associations);
  absent.closed(true);
  check(unchanged.context == original.context && unchanged.beforeRecord == original.beforeRecord &&
      unchanged.rawSettled == original.rawSettled && !absentEvidence.requested &&
      !absentEvidence.admitted && !absentEvidence.complete && !absentEvidence.closed,
      "disabled observer leaves original callback carrier and evidence untouched");

  const std::array<std::string_view, 4u> reasons{
      "v4-observer-buffer-rejected", "v4-observer-arm-rejected",
      "v4-observer-closed-rejected", "v4-observer-traversal-incomplete"};
  for (std::uint32_t fault = 0u; fault < reasons.size(); ++fault) {
    context.count = 0u;
    context.acceptObserver = false;
    auto failingObserver = observer;
    if (fault == 2u) {
      failingObserver.closed = [](void* opaque, D9CDevice*) noexcept {
        static_cast<Context*>(opaque)->append(9u);
        return false;
      };
    }
    FrameTapeReplayResult renderer;
    renderer.status = FrameTapeReplayStatus::Complete;
    render_tape_runtime::PreparedObserverV4Session failed(
        &failingObserver, renderer.preparedObserverV4, nullptr);
    if (fault == 0u) failed.buffer(event);
    if (fault == 1u) failed.armed(19, arm, root);
    failed.closed(fault != 3u);
    failed.closed(true);
    check(renderer.complete() && renderer.preparedObserverV4.closed &&
        !renderer.preparedObserverV4.complete && renderer.preparedObserverV4.closedCallbacks == 1u &&
        std::string_view(renderer.preparedObserverV4.failure) == reasons[fault] &&
        !renderer.exactDiagnosticV4.execution.complete &&
        !renderer.exactDiagnosticV4.execution.policyQualified &&
        !renderer.exactDiagnosticV4.execution.semanticMappingComplete,
        "each observer failure is sticky/incomplete and cannot alter renderer result or qualify policy");
  }
}

void normalPreparation() {
  const auto fixture = makeFixture();
  GeneralPreflightPlan general;
  check(render_tape_runtime::buildGeneralReplayPlan(fixture.tape, fixture.references, general).complete(),
      "Normal preparation uses the actual General parser");
  CaptureSourceReplayPlanV4 source;
  check(buildCaptureSourceReplayPlanV4(fixture.tape, general.catalogue, fixture.identity,
      fixture.topology, source) == CaptureSourceReplayPlanV4Status::Valid, "Normal source pair authenticates");
  const auto& h = source.identity().header;
  RenderTapeActionProfileV1ComponentHeader action;
  action.witness.evidence = static_cast<std::uint32_t>(RenderTapeActionWitnessEvidenceV1::QueueObserved);
  action.witness.captureToken = h.captureToken;
  action.witness.captureEpoch = h.captureEpoch;
  action.witness.queue = h.queue;
  action.eventsBytes = fixture.tape.size(); action.identityBytes = fixture.identity.size();
  action.executionTopologyBytes = fixture.topology.size();
  action.eventsDigest = RenderTapeCaptureSession::sha256(fixture.tape);
  action.identityDigest = RenderTapeCaptureSession::sha256(fixture.identity);
  action.executionTopologyDigest = RenderTapeCaptureSession::sha256(fixture.topology);
  RenderTapeAttachmentHistoryHeader header{.token = h.captureToken, .epoch = h.captureEpoch,
      .queue = h.queue, .completedSequence = h.baselineSequence,
      .completedInitializer = h.baselineInitializer};
  std::vector<RenderTapeAttachmentHistoryRow> rows;
  for (const auto& definition : general.definitions) {
    if (definition.fixed.identity.kind != D9C_CHUNK_HANDLE_KIND_SURFACE) continue;
    RenderTapeSurfaceDescriptorV2 desc{};
    check(renderTapeLoadSurfaceDescriptorV2(definition.descriptor, desc), "surface descriptor for original history");
    const bool alias = desc.storage == static_cast<std::uint32_t>(RenderTapeSurfaceStorage::TextureSubresource);
    const bool current = desc.storage == static_cast<std::uint32_t>(RenderTapeSurfaceStorage::SwapchainBackbuffer);
    rows.push_back({definition.fixed.identity, alias ? desc.parentTexture : definition.fixed.identity,
        desc.subresource, 1u, current ? 1u : 0u, 0u});
    if (current) header.flags |= 2u;
  }
  std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) { return a.surface.objectId < b.surface.objectId; });
  header.count = rows.size();
  std::array<std::byte, kRenderTapeActionProfileV1Bytes> actionBytes;
  std::vector<std::byte> history;
  const auto makeOptions = [&] {
    check(encodeRenderTapeActionProfileV1Component(action, actionBytes), "Normal action fixture encodes canonically");
    check(encodeRenderTapeAttachmentHistory(header, rows, history), "Normal history fixture encodes canonically");
    auto options = immutableOptionsFor(fixture);
    options.normalActionProfile = actionBytes; options.normalAttachmentHistory = history;
    options.expectedNormalActionDigest = RenderTapeCaptureSession::sha256(actionBytes);
    options.expectedNormalHistoryDigest = RenderTapeCaptureSession::sha256(history);
    return options;
  };
  auto options = makeOptions();
  auto prepared = prepareRenderTapeIdentityV4(fixture.tape, fixture.references, options);
  check(prepared.valid(), "bound unchanged-Normal companions admit immutable diagnostic preparation");
  auto result = replayPreparedRenderTapeIdentityV4(nullptr, prepared, RenderTapePreparedRoute::Compatibility);
  check(result.normalDiagnostic.requested && !result.normalDiagnostic.admitted &&
      !result.normalDiagnostic.historyRestored && !result.normalDiagnostic.unchangedNormal,
      "missing device grants no local history or policy evidence");
  noEffects(result);
  options.expectedNormalHistoryDigest[0] ^= std::byte{1};
  check(!prepareRenderTapeIdentityV4(fixture.tape, fixture.references, options).valid(), "wrong history digest rejects");
  options = makeOptions(); options.normalActionProfile = {};
  check(!prepareRenderTapeIdentityV4(fixture.tape, fixture.references, options).valid(), "partial Normal request rejects");
  action.eventsDigest[0] ^= std::byte{1}; options = makeOptions();
  check(!prepareRenderTapeIdentityV4(fixture.tape, fixture.references, options).valid(), "rehashing a foreign action component cannot repair source binding");
  action.eventsDigest[0] ^= std::byte{1};
  action.witness.armRevision = action.witness.freezeRevision = 2u; options = makeOptions();
  check(!prepareRenderTapeIdentityV4(fixture.tape, fixture.references, options).valid(), "Normal policy ABA rejects");
  action.witness.armRevision = action.witness.freezeRevision = 0u;
  const auto savedRows = rows;
  const auto savedHeader = header;
  rows.clear(); header.count = 0u; header.flags = 0u; options = makeOptions();
  check(!prepareRenderTapeIdentityV4(fixture.tape, fixture.references, options).valid(), "valid but incomplete history rejects");
  rows = savedRows; header = savedHeader;
  ++header.epoch; options = makeOptions();
  check(!prepareRenderTapeIdentityV4(fixture.tape, fixture.references, options).valid(), "foreign capture history rejects");
  --header.epoch;
  // Every synchronous allocation in the new immutable preparation is faulted.
  options = makeOptions();
  std::size_t count = 0u;
  { ScopedV4AllocationProbe probe(false);
    auto value = prepareRenderTapeIdentityV4(fixture.tape, fixture.references, options);
    count = prepared_v4_allocation_probe::count;
    check(value.valid(), "counted Normal preparation remains valid"); }
  for (std::size_t i = 0u; i < count; ++i) {
    bool valid = false, injected = false;
    { ScopedV4AllocationProbe probe(true, i);
      auto value = prepareRenderTapeIdentityV4(fixture.tape, fixture.references, options);
      valid = value.valid(); injected = prepared_v4_allocation_probe::injected; }
    check(injected && !valid, "Normal preparation allocation fault publishes no partial authority");
  }
}

void unsupportedQuery() {
  const auto fixture = makeFixture();
  ImportedRenderTapeView tape;
  check(importPrevalidatedRenderTape(fixture.tape, tape), "query fixture source imports");
  RenderTapeBuilder builder;
  for (std::uint32_t index = 0u; index < tape.events.size(); ++index) {
    const auto event = tape.event(index);
    const auto type = static_cast<RenderTapeEventType>(event.header.type);
    if (type == RenderTapeEventType::PresentComplete) {
      std::vector<std::byte> payload(event.payload.begin(), event.payload.end());
      RenderTapePresentCompleteHeader completion{};
      std::memcpy(&completion, payload.data(), sizeof(completion));
      ++completion.presentOrdinal;
      std::memcpy(payload.data(), &completion, sizeof(completion));
      builder.appendRawEvent(type, payload);
    } else {
      builder.appendRawEvent(type, event.payload);
    }
    if (type == RenderTapeEventType::BootstrapState) {
      const D9CWireObjectIdentity query{D9C_CHUNK_HANDLE_KIND_QUERY, 7u, 71u};
      const RenderTapeQueryDescriptorV2 descriptor{.type = 10u, .dataBytes = 8u};
      builder.appendObjectDefine(query, static_cast<std::uint32_t>(RenderTapeDescriptorKind::Query),
          std::as_bytes(std::span(&descriptor, 1u)), 0u, {});
    }
  }
  const auto prepared = prepareRenderTapeIdentity(builder.seal(), fixture.references);
  check(!prepared.valid() && prepared.preflight().queryReplay.failure ==
      RenderTapeQueryReplayFailure::UnsupportedResultKind,
      "absolute TIMESTAMP retains the existing unsupported-result gate");
  const auto result = replayPreparedRenderTapeIdentity(nullptr, prepared, optionsFor(fixture));
  check(!result.complete() && !result.exactDiagnosticV4.execution.validated &&
      result.queryReplay.failure == RenderTapeQueryReplayFailure::UnsupportedResultKind,
      "a V4 request cannot waive unsupported query preflight");
  noEffects(result);
  const auto immutable = prepareRenderTapeIdentityV4(builder.seal(), fixture.references,
      immutableOptionsFor(fixture));
  check(!immutable.valid() && !immutable.evidence().immutableV4PreparationOwned &&
      !immutable.evidence().v4PreparationCount,
      "unsupported query grammar cannot publish an eager immutable V4 owner or waive original query admission");
}

void terminalOutputMismatch() {
  constexpr std::uint32_t terminalEvent = 100u;
  FrameTapeValidityEvidence pixels;
  pixels.structurallyValid = true;
  pixels.digestsValid = true;
  pixels.outputReadback = true;
  pixels.expectedDigestCaptured = true;
  pixels.outputBytes = 8u;
  pixels.expectedOutputDigest[0] = std::byte{1u};
  pixels.outputDigest[0] = std::byte{2u};
  pixels.sourceReadback = true;
  pixels.sourceBytes = pixels.outputBytes;
  pixels.sourceDigest = pixels.outputDigest;

  for (const auto mode : {Mode::Chunk, Mode::CaptureSource, Mode::CaptureSourceV3,
                         Mode::CaptureSourceV4ExactDiagnostic}) {
    for (const auto type : {RenderTapeEventType::CommandChunk,
                           RenderTapeEventType::OrderedControl,
                           RenderTapeEventType::PresentComplete}) {
      for (const auto event : {terminalEvent - 1u, terminalEvent}) {
        const bool privateV4 = mode == Mode::CaptureSourceV4ExactDiagnostic;
        check(renderTapeV4TerminalOutputMismatchMayDefer(
            privateV4, type, event, terminalEvent, pixels) ==
            (privateV4 && type == RenderTapeEventType::PresentComplete &&
             event == terminalEvent),
            "shared production predicate defers only private V4 terminal PresentComplete");
      }
    }
  }
  for (std::uint32_t fault = 0u; fault != 4u; ++fault) {
    auto invalid = pixels;
    if (fault == 0u) invalid.outputReadback = false;
    if (fault == 1u) invalid.expectedDigestCaptured = false;
    if (fault == 2u) invalid.expectedDigestMatched = true;
    const auto event = fault == 3u ? 0xffffffffu : terminalEvent;
    check(!renderTapeV4TerminalOutputMismatchMayDefer(true,
        RenderTapeEventType::PresentComplete, event, event, invalid),
        "missing readback/digest, matching pixels or invalid event cannot create deferred rejection");
  }

  // These are authored reducer inputs, not a claim of actual GPU completion.
  // The provider uses these same pure helpers around its real finalizers.
  const auto makeResult = [&] {
    FrameTapeReplayResult result;
    result.status = FrameTapeReplayStatus::Complete;
    result.validity = pixels;
    result.intervals[0].validity = pixels;
    result.outputPixels.assign(8u, std::byte{2u});
    result.sourcePixels.assign(8u, std::byte{2u});
    result.conservation.objectsCreated = 8u;
    result.conservation.objectsReleased = 8u;
    result.exactDiagnosticV4.execution.requested = true;
    return result;
  };
  const auto preservedPixelsAndGates = [&](const FrameTapeReplayResult& result) {
    const auto& execution = result.exactDiagnosticV4.execution;
    check(result.validity.outputReadback && result.validity.sourceReadback &&
        result.validity.expectedDigestCaptured && !result.validity.expectedDigestMatched &&
        !result.validity.expectedPixelsCompared && !result.validity.pixelEnvelopeMatched &&
        result.validity.outputBytes == 8u && result.validity.sourceBytes == 8u &&
        result.validity.expectedOutputDigest == pixels.expectedOutputDigest &&
        result.validity.outputDigest == pixels.outputDigest &&
        result.validity.sourceDigest == pixels.sourceDigest &&
        !result.intervals[0].validity.expectedDigestMatched &&
        result.outputPixels.size() == 8u && result.sourcePixels.size() == 8u &&
        result.outputPixels[0] == std::byte{2u} && result.sourcePixels[0] == std::byte{2u},
        "deferred rejection preserves strict mismatch and both readback artifacts");
    check(execution.qualifierOnly && !execution.policyQualified &&
        !execution.queryReadinessQualified && !execution.pixelsQualified &&
        !execution.timingEligible && !result.sourceReplay.complete &&
        !result.sourceReplayV3.complete &&
        result.conservation.objectsCreated == result.conservation.objectsReleased,
        "pixel deferral cannot promote policy/timing or disturb conservation");
  };

  auto finalized = makeResult();
  auto& execution = finalized.exactDiagnosticV4.execution;
  execution.complete = true;
  execution.canonicalTraversalComplete = true;
  execution.rawAckCoverageComplete = true;
  execution.actualLifecycleComplete = true;
  execution.actualTopologyComplete = true;
  execution.exactSourceDiagnosticComplete = true;
  finalized.queryReplay.present = true;
  finalized.queryReplay.transcriptComplete = true;
  finalized.queryReplay.oracleComplete = true;
  finalized.queryReplay.readinessRequired = 1u;
  finalized.queryReplay.readinessSatisfied = 1u;
  finalized.queryReplay.rows.push_back({.dataSize = 4u, .definedBytes = 0u,
      .resultRequired = true, .readinessSatisfied = true});
  const auto* outputStorage = finalized.outputPixels.data();
  const auto* sourceStorage = finalized.sourcePixels.data();
  renderTapeReduceDeferredOutputMismatch(finalized, terminalEvent);
  check(finalized.status == FrameTapeReplayStatus::OutputMismatch &&
      finalized.failedEventIndex == terminalEvent && !finalized.complete() &&
      execution.complete && execution.canonicalTraversalComplete &&
      execution.rawAckCoverageComplete && execution.actualLifecycleComplete &&
      execution.actualTopologyComplete && execution.exactSourceDiagnosticComplete &&
      std::string_view(finalized.exactDiagnosticV4.failure) == "none",
      "successful independent finalization still rejects the original mismatched pixels");
  check(finalized.queryReplay.transcriptComplete && finalized.queryReplay.oracleComplete &&
      finalized.queryReplay.readinessRequired == 1u &&
      finalized.queryReplay.readinessSatisfied == 1u &&
      finalized.queryReplay.requiredResults == 0u && finalized.queryReplay.matchedResults == 0u &&
      finalized.queryReplay.rows[0].definedBytes == 0u &&
      finalized.queryReplay.rows[0].readinessSatisfied &&
      !finalized.queryReplay.rows[0].resultMatched &&
      finalized.outputPixels.data() == outputStorage &&
      finalized.sourcePixels.data() == sourceStorage,
      "readiness-only EVENT evidence and original pixel storage survive without invented bytes");
  preservedPixelsAndGates(finalized);

  for (const auto status : {FrameTapeReplayStatus::QueryOracleFailed,
                           FrameTapeReplayStatus::QueryProgressExhausted,
                           FrameTapeReplayStatus::CommandReplayFailed,
                           FrameTapeReplayStatus::PresentOutputFailed,
                           FrameTapeReplayStatus::ObjectCreationFailed}) {
    auto failed = makeResult();
    failed.status = status;
    failed.failedEventIndex = status == FrameTapeReplayStatus::CommandReplayFailed
        ? 0xffffffffu : 17u;
    failed.exactDiagnosticV4.failure = "authored-independent-finalization-failure";
    if (status == FrameTapeReplayStatus::QueryOracleFailed) {
      failed.queryReplay.present = true;
      failed.queryReplay.failure = RenderTapeQueryReplayFailure::ResultMismatch;
      failed.queryReplay.requiredResults = 1u;
    } else if (status == FrameTapeReplayStatus::QueryProgressExhausted) {
      failed.queryReplay.present = true;
      failed.queryReplay.failure = RenderTapeQueryReplayFailure::ProgressExhausted;
      failed.queryReplay.readinessRequired = 1u;
    }
    const auto originalEvent = failed.failedEventIndex;
    const auto queryFailure = failed.queryReplay.failure;
    renderTapeReduceDeferredOutputMismatch(failed, terminalEvent);
    check(failed.status == status && failed.failedEventIndex == originalEvent &&
        std::string_view(failed.exactDiagnosticV4.failure) ==
            "authored-independent-finalization-failure" &&
        failed.queryReplay.failure == queryFailure && !failed.queryReplay.oracleComplete &&
        failed.queryReplay.matchedResults == 0u &&
        failed.queryReplay.readinessSatisfied == 0u &&
        !failed.exactDiagnosticV4.execution.complete &&
        !failed.exactDiagnosticV4.execution.actualTopologyComplete &&
        !failed.exactDiagnosticV4.execution.rawAckCoverageComplete,
        "concrete query/ack/source/topology/output/cleanup failure outranks known pixel mismatch");
    preservedPixelsAndGates(failed);
  }

  auto noDeferredEvent = makeResult();
  renderTapeReduceDeferredOutputMismatch(noDeferredEvent, 0xffffffffu);
  check(noDeferredEvent.complete() && noDeferredEvent.failedEventIndex == 0xffffffffu,
      "ordinary/V3 paths without a deferred event retain their existing final status");
  auto incomplete = makeResult();
  renderTapeReduceDeferredOutputMismatch(incomplete, terminalEvent);
  check(incomplete.status == FrameTapeReplayStatus::OutputMismatch &&
      !incomplete.exactDiagnosticV4.execution.complete &&
      !incomplete.exactDiagnosticV4.execution.canonicalTraversalComplete &&
      !incomplete.exactDiagnosticV4.execution.rawAckCoverageComplete &&
      !incomplete.exactDiagnosticV4.execution.actualLifecycleComplete &&
      !incomplete.exactDiagnosticV4.execution.actualTopologyComplete &&
      !incomplete.exactDiagnosticV4.execution.exactSourceDiagnosticComplete,
      "reducing pixel rejection never fabricates missing independent completion evidence");
  preservedPixelsAndGates(incomplete);
}

} // namespace

int main() {
  try {
    admission();
    immutablePreparedInputLifetime();
    immutablePreparedFactoryFailures();
    modes();
    observerAdmission();
    observerForwarding();
    normalPreparation();
    unsupportedQuery();
    terminalOutputMismatch();
    std::cout << "render_tape_provider_v4_spec: " << checks << " checks passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "render_tape_provider_v4_spec: " << error.what() << '\n';
    return 1;
  }
}
