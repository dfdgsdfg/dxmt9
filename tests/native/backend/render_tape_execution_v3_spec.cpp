#include "device_c_render_tape_execution_v3.hpp"
#include "device_c_render_tape_runtime_objects.hpp"
#include "render_tape_completion_fixture.hpp"
#include "render_tape_cpu_destroy_fixture.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace dxmt9::d3d9;
using render_tape_runtime::GeneralPreflightPlan;

void check(bool condition, std::string_view message) {
  if (!condition) throw std::runtime_error(std::string(message));
}

struct AdmissionFixture {
  std::vector<std::byte> tape{};
  std::vector<std::vector<std::byte>> blobs{};
  std::vector<std::byte> sidecar{};
  GeneralPreflightPlan replay{};
  CaptureSourceReplayPlanV3 sourcePlan{};
  std::uint32_t expectedPrefix = 0u;
};

AdmissionFixture makeFixture(bool seeded = false, bool ordinaryMutation = false,
                             bool publicationInsidePrefix = false,
                             bool orderedControl = false) {
  AdmissionFixture fixture;
  auto original = test::makeGeneralCpuDestroyFixture(false, false);
  ImportedRenderTapeView imported;
  check(importPrevalidatedRenderTape(original.tape, imported),
        "execution fixture retains the genuine indexed-draw resource closure");
  if (seeded) {
    fixture.blobs = std::move(original.blobs);
  } else {
    // No initial contents are promised by these buffer definitions. This
    // variant qualifies setup only; it never executes an undefined draw.
    fixture.blobs.assign(original.blobs.begin() + 2u, original.blobs.end());
  }
  RenderTapeBuilder builder;
  std::uint64_t ordinal = 0u;
  std::uint64_t presentEvent = 0u;
  bool firstCommand = true;
  for (std::size_t i = 0u; i < imported.events.size(); ++i) {
    const auto event = imported.event(i);
    const auto type = static_cast<RenderTapeEventType>(event.header.type);
    if (type == RenderTapeEventType::ObjectDefine && !seeded) {
      std::vector<std::byte> payload(event.payload.begin(), event.payload.end());
      RenderTapeObjectDefineHeader definition{};
      std::memcpy(&definition, payload.data(), sizeof(definition));
      if (definition.identity.kind == D9C_CHUNK_HANDLE_KIND_BUFFER) {
        definition.expectedContentBytes = 0u;
        definition.expectedContentCount = 0u;
        std::memcpy(payload.data(), &definition, sizeof(definition));
      }
      builder.appendRawEvent(type, payload);
    } else if (type == RenderTapeEventType::ResourceMutation && !seeded) {
      continue;
    } else if (type == RenderTapeEventType::CommandChunk) {
      if (firstCommand) {
        fixture.expectedPrefix = static_cast<std::uint32_t>(ordinal);
        if (ordinaryMutation) {
          const auto& bytes = fixture.blobs[0];
          builder.appendResourceMutation(
              test::general_cpu_destroy_fixture_detail::kVertexBuffer,
              RenderTapeMutationKind::CpuUnlock, 0u, 0u, bytes.size(),
              RenderTapeCaptureSession::sha256(bytes));
          ++ordinal;
        }
        if (orderedControl) {
          const std::vector<std::byte> gamma(kRenderTapeGammaRampBytes);
          builder.appendOrderedControl(
              {.kind = static_cast<std::uint32_t>(RenderTapeControlKind::GammaRampSet),
               .disposition = static_cast<std::uint32_t>(RenderTapeControlDisposition::Completed),
               .controlBytes = kRenderTapeGammaRampBytes}, gamma);
          ++ordinal;
        }
        builder.appendCommandChunk({.recordCount = 1u}, test::fullStateChunk(false));
        ++ordinal;
        firstCommand = false;
      }
      RenderTapeCommandChunkHeader header{};
      std::memcpy(&header, event.payload.data(), sizeof(header));
      ImportedChunkView chunk;
      check(importPrevalidatedCommandChunk(event.payload.subspan(sizeof(header)),
                {header.wireVersion, header.recordCount, header.handleCount}, chunk),
            "execution fixture retains each original strict wire chunk");
      if (chunk.records[0].type == D9C_COMMAND_RECORD_PRESENT)
        presentEvent = ordinal + 1u;
      builder.appendRawEvent(type, event.payload);
    } else if (type == RenderTapeEventType::PresentComplete) {
      std::vector<std::byte> payload(event.payload.begin(), event.payload.end());
      RenderTapePresentCompleteHeader completion{};
      std::memcpy(&completion, payload.data(), sizeof(completion));
      completion.presentOrdinal = presentEvent;
      std::memcpy(payload.data(), &completion, sizeof(completion));
      builder.appendRawEvent(type, payload);
    } else {
      builder.appendRawEvent(type, event.payload);
    }
    ++ordinal;
  }
  fixture.tape = builder.seal();
  std::vector<RenderTapeProviderBlob> blobs;
  for (const auto& bytes : fixture.blobs)
    blobs.push_back({RenderTapeCaptureSession::sha256(bytes), bytes});
  check(render_tape_runtime::buildGeneralReplayPlan(
            fixture.tape, blobs, fixture.replay).complete(),
        "execution fixture uses the complete production original-tape preflight");

  RenderTapeIdentityV3View identity;
  identity.header.frameId = 3u;
  identity.header.captureToken = 5u;
  identity.header.captureEpoch = 6u;
  identity.header.presentOrdinal = presentEvent;
  identity.header.queue = 1u;
  identity.header.initializerEpoch = 1u;
  identity.header.baselineCompletionEdge = 1u;
  identity.header.baselineKind = static_cast<std::uint32_t>(
      RenderTapeIdentityV3PrefixKind::EmptyBaseline);
  std::uint64_t edge = publicationInsidePrefix ? 5u : 2u;
  for (const auto& event : fixture.replay.events) {
    if (event.type != RenderTapeEventType::CommandChunk) continue;
    const auto eventOrdinal = fixture.replay.tape.event(event.eventIndex).header.ordinal;
    const auto& originalChunk = fixture.replay.commandChunks[event.index];
    ImportedChunkView chunk;
    check(importPrevalidatedCommandChunk(originalChunk.bytes, originalChunk.envelope, chunk),
          "execution fixture maps exact production record types");
    const auto recordType = chunk.records[0].type;
    if (recordType == D9C_COMMAND_RECORD_APPLY_STATE) {
      identity.dispositions.push_back({.eventOrdinal = eventOrdinal, .recordCount = 1u,
          .kind = static_cast<std::uint32_t>(RenderTapeIdentityV3DispositionKind::StateOnly)});
    } else {
      const bool present = recordType == D9C_COMMAND_RECORD_PRESENT;
      const auto sourceIndex = static_cast<std::uint32_t>(identity.sources.size());
      const auto commandIndex = static_cast<std::uint32_t>(identity.commands.size());
      const auto sequence = std::uint64_t{sourceIndex} + 1u;
      identity.sources.push_back({.sourceOrdinal = sequence, .seqId = sequence,
          .writerCaptureId = sequence, .writerGeneration = 2u, .storageGeneration = 3u,
          .publicationEdge = edge++, .submissionEdge = edge++, .completionEdge = edge++,
          .completedSequence = sequence, .cutEventOrdinal = eventOrdinal, .commandCount = 1u,
          .cutKind = static_cast<std::uint32_t>(RenderTapeIdentityV3CutKind::AfterEvent)});
      identity.commands.push_back({.logicalPassId = present ? 0u : 17u,
          .sourceIndex = sourceIndex,
          .kind = static_cast<std::uint32_t>(present ? RenderTapeIdentityV3CommandKind::Present
                                                    : RenderTapeIdentityV3CommandKind::DrawRun),
          .drawCount = present ? 0u : 1u,
          .membership = static_cast<std::uint32_t>(present
              ? RenderTapeIdentityV3Membership::NoPassCommand
              : RenderTapeIdentityV3Membership::Pass),
          .dagPassIndex = present ? kRenderTapeIdentityV3NoIndex : 0u, .passKind = present ? 0u : 1u});
      identity.dispositions.push_back({.eventOrdinal = eventOrdinal, .recordCount = 1u,
          .kind = static_cast<std::uint32_t>(RenderTapeIdentityV3DispositionKind::GpuCommand),
          .commandIndex = commandIndex, .subdrawCount = present ? 0u : 1u});
    }
    identity.settlements.push_back({eventOrdinal, eventOrdinal, 1u, edge++, 1u});
  }
  if (publicationInsidePrefix) {
    identity.initializers = {{.sequence = 1u, .epoch = 1u,
        .publicationEdge = 2u, .submissionEdge = 3u, .completionEdge = 4u,
        .completedSequence = 1u, .cutEventOrdinal = 2u,
        .cutKind = static_cast<std::uint32_t>(RenderTapeIdentityV3CutKind::AfterEvent)}};
  }
  RenderTapeIdentityV3Status identityStatus{};
  fixture.sidecar = buildRenderTapeIdentityV3(
      fixture.tape, fixture.replay.catalogue, identity, &identityStatus);
  check(identityStatus == RenderTapeIdentityV3Status::Valid &&
            buildCaptureSourceReplayPlanV3(fixture.tape, fixture.replay.catalogue,
                fixture.sidecar, fixture.sourcePlan) == CaptureSourceReplayPlanV3Status::Valid,
        "execution admission consumes the production authenticated complete source plan");
  return fixture;
}

void checkNoExecution(const RenderTapeSourceReplayV3Evidence& evidence) {
  check(!evidence.executed && !evidence.complete && !evidence.lifecycleComplete &&
            !evidence.timingEligible && evidence.dispatchedRecords == 0u &&
            evidence.replayedEvents == 0u && evidence.externalPublicationCuts == 0u &&
            evidence.publications.empty() && evidence.initializers.empty() &&
            evidence.settlements.empty(),
        "pure admission records no replay, publication, submission, completion or timing evidence");
}

void validCompatibilityAdmission() {
  for (std::uint32_t variant = 0u; variant < 3u; ++variant) {
    auto fixture = makeFixture(variant != 0u, variant == 2u);
    RenderTapeExecutionV3 execution;
    RenderTapeSourceReplayV3Evidence evidence;
    check(execution.admit(fixture.sourcePlan, fixture.replay, fixture.tape, evidence) &&
              !execution.failed() && std::string_view(execution.failure()) == "none" &&
              execution.snapshotPrefixEvents() == fixture.expectedPrefix &&
              evidence.requested && evidence.validated && evidence.compatibility &&
              evidence.qualifierOnly && evidence.planDigest == fixture.sourcePlan.digest(),
          "complete immutable plan admits compatibility after exact setup-prefix qualification");
    checkNoExecution(evidence);
    check(!execution.arm(nullptr) && execution.failed() &&
              std::string_view(execution.failure()) == "v3-arm-input",
          "validated capture labels do not authorize execution without a local device");
    checkNoExecution(evidence);
  }
}

void invalidInputsRejectBeforeEffects() {
  for (std::uint32_t mutation = 0u; mutation < 8u; ++mutation) {
    auto fixture = makeFixture();
    CaptureSourceReplayPlanV3 invalid;
    auto* plan = &fixture.sourcePlan;
    auto tape = std::span<const std::byte>(fixture.tape);
    std::vector<std::byte> changed;
    auto expected = std::string_view("v3-exact-tape-binding");
    if (mutation == 0u) { plan = &invalid; expected = "v3-plan-input"; }
    if (mutation == 1u) {
      fixture.replay.events.pop_back();
      expected = "v3-plan-input";
    }
    if (mutation == 2u) {
      changed = fixture.tape;
      changed.back() ^= std::byte{1};
      tape = changed;
    }
    if (mutation == 3u) ++fixture.replay.tape.header.eventCount;
    if (mutation == 4u) {
      fixture.replay.events.back().eventIndex = UINT32_MAX;
      expected = "v3-event-binding";
    }
    if (mutation == 5u) {
      ++fixture.replay.commandChunks[0].envelope.recordCount;
      expected = "v3-event-binding";
    }
    if (mutation == 6u) {
      const auto bytes = fixture.replay.commandChunks[0].bytes;
      changed.assign(bytes.begin(), bytes.end());
      changed.back() ^= std::byte{1};
      fixture.replay.commandChunks[0].bytes = changed;
      expected = "v3-exact-command-binding";
    }
    if (mutation == 7u) tape = tape.first(tape.size() - 1u);
    RenderTapeExecutionV3 execution;
    RenderTapeSourceReplayV3Evidence evidence;
    check(!execution.admit(*plan, fixture.replay, tape, evidence) && execution.failed() &&
              std::string_view(execution.failure()) == expected && !evidence.validated,
          "invalid plan, immutable bytes and event/chunk rebinding fail before local effects");
    checkNoExecution(evidence);
  }
  auto fixture = makeFixture();
  RenderTapeExecutionV3 execution;
  RenderTapeSourceReplayV3Evidence evidence;
  check(execution.admit(fixture.sourcePlan, fixture.replay, fixture.tape, evidence) &&
            !execution.admit(fixture.sourcePlan, fixture.replay, fixture.tape, evidence) &&
            std::string_view(execution.failure()) == "v3-plan-input",
        "an execution owner cannot admit a second plan over the same immutable input");
  checkNoExecution(evidence);
}

void invalidSetupPrefixRejectsBeforeEffects() {
  for (std::uint32_t mutation = 0u; mutation < 7u; ++mutation) {
    auto fixture = makeFixture(true);
    auto& definition = fixture.replay.definitions[1].fixed;
    auto& mutationSeed = fixture.replay.mutations[0];
    if (mutation == 0u) mutationSeed.byteOffset = 1u;
    if (mutation == 1u) ++mutationSeed.byteSize;
    if (mutation == 2u) ++definition.expectedContentBytes;
    if (mutation == 3u) ++definition.expectedContentCount;
    if (mutation == 4u) ++mutationSeed.identity.generation;
    if (mutation == 5u) fixture.replay.events[6].index = UINT32_MAX;
    if (mutation == 6u) {
      definition.expectedContentCount = 2u;
      fixture.replay.events[8].type = RenderTapeEventType::ResourceMutation;
      fixture.replay.events[8].index = 0u;
    }
    RenderTapeExecutionV3 execution;
    RenderTapeSourceReplayV3Evidence evidence;
    check(!execution.admit(fixture.sourcePlan, fixture.replay, fixture.tape, evidence) &&
              std::string_view(execution.failure()) == "v3-snapshot-prefix" &&
              !evidence.validated,
          "partial, offset, missing, stale or duplicate initial seeds cannot become completed setup");
    checkNoExecution(evidence);
  }
}

void unsupportedPublicationCutRejectsBeforeEffects() {
  auto fixture = makeFixture(false, false, true);
  check(fixture.sourcePlan.valid() && fixture.sourcePlan.identity().initializers.size() == 1u,
        "unsupported execution cut remains an otherwise complete authenticated v3 source plan");
  RenderTapeExecutionV3 execution;
  RenderTapeSourceReplayV3Evidence evidence;
  check(!execution.admit(fixture.sourcePlan, fixture.replay, fixture.tape, evidence) &&
            execution.failed() && std::string_view(execution.failure()) ==
                "v3-publication-inside-snapshot-prefix" && !evidence.validated,
        "an original publication inside setup cannot be silently replaced by a replay seed drain");
  checkNoExecution(evidence);
}

void boundedRuntimeIdentityRejectsBeforeEffects() {
  auto fixture = makeFixture();
  auto identity = fixture.sourcePlan.identity();
  const auto count = RenderTapeIdentityV3Limits{}.maxInitializers + 1u;
  identity.initializers.resize(count);
  const auto stateOrdinal = fixture.sourcePlan.events()[fixture.expectedPrefix].eventOrdinal;
  for (std::uint32_t i = 0u; i < count; ++i) {
    const std::uint64_t edge = 3u + std::uint64_t{i} * 3u;
    identity.initializers[i] = {.sequence = std::uint64_t{i} + 1u, .epoch = 1u,
        .publicationEdge = edge, .submissionEdge = edge + 1u,
        .completionEdge = edge + 2u, .completedSequence = std::uint64_t{i} + 1u,
        .cutEventOrdinal = stateOrdinal,
        .cutKind = static_cast<std::uint32_t>(RenderTapeIdentityV3CutKind::AfterEvent)};
  }
  for (std::size_t i = 0u; i < identity.sources.size(); ++i) {
    auto& source = identity.sources[i];
    source.publicationEdge = 3u + std::uint64_t{count} * 3u + i * 4u;
    source.submissionEdge = source.publicationEdge + 1u;
    source.completionEdge = source.publicationEdge + 2u;
    identity.settlements[i + 1u].replayEdge = source.publicationEdge + 3u;
  }
  RenderTapeIdentityV3Status identityStatus{};
  const auto sidecar = buildRenderTapeIdentityV3(
      fixture.tape, fixture.replay.catalogue, identity, &identityStatus);
  CaptureSourceReplayPlanV3 excessive;
  check(identityStatus == RenderTapeIdentityV3Status::Valid &&
            buildCaptureSourceReplayPlanV3(fixture.tape, fixture.replay.catalogue,
                sidecar, excessive) == CaptureSourceReplayPlanV3Status::Valid,
        "wire-valid identity fixture reaches the narrower bounded runtime admission");
  RenderTapeExecutionV3 execution;
  RenderTapeSourceReplayV3Evidence evidence;
  check(!execution.admit(excessive, fixture.replay, fixture.tape, evidence) &&
            std::string_view(execution.failure()) == "v3-runtime-identity-capacity" &&
            !evidence.validated,
        "runtime association capacity cannot be bypassed by a larger authenticated wire table");
  checkNoExecution(evidence);
}

void canonicalGeneralPlanBindingRejectsBeforeEffects() {
  auto valid = makeFixture(true, true, false, true);
  std::reverse(valid.replay.catalogue.blobs.begin(), valid.replay.catalogue.blobs.end());
  RenderTapeExecutionV3 admitted;
  RenderTapeSourceReplayV3Evidence admittedEvidence;
  check(admitted.admit(valid.sourcePlan, valid.replay, valid.tape, admittedEvidence),
        "authenticated catalogue order is not a semantic replay dependency");
  checkNoExecution(admittedEvidence);
  CaptureSourceReplayPlanV3 unauthenticated;
  check(!render_tape_runtime::generalReplayPlanMatchesAuthenticatedV3(
            valid.tape, unauthenticated, valid.replay),
        "an unadmitted owner cannot upgrade supplied catalogue verification flags");

  for (std::uint32_t mutation = 0u; mutation < 38u; ++mutation) {
    auto fixture = makeFixture(true, true, false, true);
    check(fixture.replay.mutations.size() == 3u && fixture.replay.controls.size() == 1u &&
              fixture.replay.destroys.size() == 2u && !fixture.replay.bootstrapChunks.empty(),
          "canonical binding fixture carries seeds, ordinary mutation, control and destroy tables");
    std::vector<std::byte> changed;
    const auto eventOf = [&](RenderTapeEventType type) -> GeneralPreflightPlan::ReplayEvent& {
      const auto it = std::find_if(fixture.replay.events.begin(), fixture.replay.events.end(),
          [&](const auto& event) { return event.type == type; });
      check(it != fixture.replay.events.end(), "binding fixture retains the selected event mapping");
      return *it;
    };
    switch (mutation) {
    case 0u: ++fixture.replay.definitions[0].fixed.identity.generation; break;
    case 1u: ++fixture.replay.definitions[0].fixed.descriptorKind; break;
    case 2u: fixture.replay.definitions[0].fixed.immutablePayloadDigest[0] ^= std::byte{1}; break;
    case 3u: {
      const auto descriptor = fixture.replay.definitions[0].descriptor;
      changed.assign(descriptor.begin(), descriptor.end());
      changed.back() ^= std::byte{1};
      fixture.replay.definitions[0].descriptor = changed;
      break;
    }
    case 4u:
      eventOf(RenderTapeEventType::ObjectDefine).index =
          static_cast<std::uint32_t>(fixture.replay.definitions.size() - 1u);
      break;
    case 5u: ++eventOf(RenderTapeEventType::ObjectDestroy).index; break;
    case 6u: ++fixture.replay.mutations.back().byteOffset; break;
    case 7u: ++fixture.replay.mutations.back().bufferDisposition; break;
    case 8u: fixture.replay.mutations.back().digest[0] ^= std::byte{1}; break;
    case 9u: ++fixture.replay.bootstrapChunks[0].envelope.producerIdentity.firstEventOrdinal; break;
    case 10u: {
      const auto bytes = fixture.replay.bootstrapChunks[0].bytes;
      changed.assign(bytes.begin(), bytes.end());
      changed.back() ^= std::byte{1};
      fixture.replay.bootstrapChunks[0].bytes = changed;
      break;
    }
    case 11u: fixture.replay.bootstrapGamma[0].push_back(std::byte{1}); break;
    case 12u: ++fixture.replay.bootstrapIdentities.back().generation; break;
    case 13u: ++eventOf(RenderTapeEventType::BootstrapState).count; break;
    case 14u: ++eventOf(RenderTapeEventType::BootstrapState).aux; break;
    case 15u: ++fixture.replay.controls[0].resultCode; break;
    case 16u: fixture.replay.controlBytes[0][0] ^= std::byte{1}; break;
    case 17u: fixture.replay.controls.clear(); break;
    case 18u: ++fixture.replay.outputIdentity.generation; break;
    case 19u: ++fixture.replay.outputDesc.width; break;
    case 20u: fixture.replay.expectedDigestValidity = RenderTapeDigestValidity::Sha256; break;
    case 21u: fixture.replay.expectedDigest[0] ^= std::byte{1}; break;
    case 22u: ++fixture.replay.presentOrdinal; break;
    case 23u: ++fixture.replay.completionOrdinal; break;
    case 24u: ++fixture.replay.catalogue.blobs[0].size; break;
    case 25u: fixture.replay.catalogue.blobs[0].verified = 0u; break;
    case 26u: ++fixture.replay.catalogue.blobs[0].reserved0; break;
    case 27u: fixture.replay.catalogue.blobs.push_back(fixture.replay.catalogue.blobs[0]); break;
    case 28u: fixture.replay.mutations.push_back(fixture.replay.mutations.back()); break;
    case 29u: fixture.replay.definitions.push_back(fixture.replay.definitions.back()); break;
    case 30u: fixture.replay.commandChunks.push_back(fixture.replay.commandChunks.back()); break;
    case 31u: ++fixture.replay.destroys[0].identity.generation; break;
    case 32u: ++fixture.replay.definitions[0].fixed.reserved0; break;
    case 33u: ++fixture.replay.definitions[0].fixed.reserved1; break;
    case 34u: ++fixture.replay.controls[0].reserved0; break;
    case 35u: ++fixture.replay.controls[0].reserved1; break;
    case 36u: ++fixture.replay.bootstrapChunks[0].envelope.producerIdentity.lastSourceOrdinal; break;
    case 37u: ++fixture.replay.outputDesc.multiSampleQuality; break;
    }
    RenderTapeExecutionV3 execution;
    RenderTapeSourceReplayV3Evidence evidence;
    check(!execution.admit(fixture.sourcePlan, fixture.replay, fixture.tape, evidence) &&
              execution.failed() && std::string_view(execution.failure()) ==
                  "v3-general-plan-binding" && !evidence.validated,
          "exact tape bytes do not authorize a modified derived General replay plan");
    checkNoExecution(evidence);
  }
}

} // namespace

int main() {
  try {
    validCompatibilityAdmission();
    invalidInputsRejectBeforeEffects();
    invalidSetupPrefixRejectsBeforeEffects();
    unsupportedPublicationCutRejectsBeforeEffects();
    boundedRuntimeIdentityRejectsBeforeEffects();
    canonicalGeneralPlanBindingRejectsBeforeEffects();
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "render tape v3 execution admission spec failed: " << error.what() << '\n';
    return 1;
  }
}
