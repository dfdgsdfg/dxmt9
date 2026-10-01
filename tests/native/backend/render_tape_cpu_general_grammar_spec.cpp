#include "render_tape_cpu_general_grammar.hpp"

#include "render_tape_completion_fixture.hpp"

#include <array>
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
using namespace dxmt9::d3d9::render_tape_runtime;

struct TestFailure : std::runtime_error {
  using std::runtime_error::runtime_error;
};

void check(bool condition, std::string_view message) {
  if (!condition) throw TestFailure(std::string(message));
}

struct MiniPlan {
  GeneralPreflightPlan plan{};
  std::vector<std::byte> bootstrapBytes;
  std::vector<std::byte> presentBytes;
  RenderTapeSurfaceDescriptorV2 outputDescriptor{};

  MiniPlan() {
    const D9CWireObjectIdentity output{
        .kind = D9C_CHUNK_HANDLE_KIND_SURFACE,
        .generation = 1u,
        .objectId = 1u};
    outputDescriptor = {
        .schemaVersion = kRenderTapeSurfaceDescriptorVersion2,
        .storage = static_cast<std::uint32_t>(
            RenderTapeSurfaceStorage::SwapchainBackbuffer),
        .initialContentDisposition = static_cast<std::uint32_t>(
            RenderTapeInitialContentDisposition::ProducedPresentOutput),
        .surface = D9CSurfaceDesc{
            .format = 21u,
            .resourceType = 1u,
            .usage = 1u,
            .width = 4u,
            .height = 4u,
            .depth = 1u}};
    bootstrapBytes = test::fullStateChunk(false);
    presentBytes = test::presentChunk();
    plan.outputIdentity = output;
    plan.outputDesc = outputDescriptor.surface;
    plan.bootstrapGamma.emplace_back();
    plan.bootstrapChunks.push_back({
        .envelope = CommandChunkEnvelope{
            .version = D9C_COMMAND_CHUNK_WIRE_VERSION,
            .recordCount = 1u,
            .handleCount = 0u},
        .bytes = bootstrapBytes});
    plan.commandChunks.push_back({
        .envelope = CommandChunkEnvelope{
            .version = D9C_COMMAND_CHUNK_WIRE_VERSION,
            .recordCount = 1u,
            .handleCount = 0u},
        .bytes = presentBytes});
    plan.definitions.push_back({
        .fixed = RenderTapeObjectDefineHeader{
            .identity = output,
            .descriptorKind = static_cast<std::uint32_t>(
                RenderTapeDescriptorKind::Surface),
            .descriptorBytes = sizeof(outputDescriptor)},
        .descriptor = std::as_bytes(std::span(&outputDescriptor, 1u))});
    plan.events = {
        {.type = RenderTapeEventType::BootstrapState,
         .eventIndex = 0u,
         .index = 0u,
         .count = 1u,
         .aux = 0u},
        {.type = RenderTapeEventType::ObjectDefine,
         .eventIndex = 1u,
         .index = 0u},
        {.type = RenderTapeEventType::CommandChunk,
         .eventIndex = 2u,
         .index = 0u},
        {.type = RenderTapeEventType::PresentComplete,
         .eventIndex = 3u},
    };
  }
};

GeneralCpuAdmissionResult admit(const MiniPlan& fixture) {
  return admitGeneralCpuPlan(fixture.plan);
}

void acceptedPlanRetainsGeneralObjectsAndState() {
  check(detail::generalCpuCommandRecordSupported(
            D9C_COMMAND_RECORD_GENERATE_MIPMAPS),
        "canonical mip generation has a CPU logical-operation observer");
  check(detail::generalCpuCommandRecordSupported(D9C_COMMAND_RECORD_QUERY_ISSUE),
        "canonical query issue retains the production command-record path");
  const MiniPlan fixture;
  const auto result = admit(fixture);
  check(result.accepted(), "small already-admitted plan is accepted");
  check(result.counts.events == 4u && result.counts.objects == 1u &&
            result.counts.mutations == 0u && result.counts.chunks == 1u &&
            result.counts.records == 1u && result.counts.bootstrapChunks == 1u &&
            result.counts.bootstrapRecords == 1u && result.counts.presents == 1u &&
            result.counts.presentCompletions == 1u,
        "admission reports complete plan counts");
}

void aggregateBoundsAreCheckedWithNamedReasons() {
  {
    MiniPlan fixture;
    auto caps = GeneralCpuAdmissionCaps{};
    caps.maxEvents = 3u;
    const auto result = admitGeneralCpuPlan(fixture.plan, caps);
    check(result.reason == GeneralCpuAdmissionReason::EventLimit &&
              result.eventIndex == 3u,
          "event bound reports the first over-bound event");
  }
  {
    MiniPlan fixture;
    auto caps = GeneralCpuAdmissionCaps{};
    caps.maxObjects = 0u;
    const auto result = admitGeneralCpuPlan(fixture.plan, caps);
    check(result.reason == GeneralCpuAdmissionReason::ObjectLimit &&
              result.eventIndex == 1u,
          "object bound reports its first object event");
  }
  {
    MiniPlan fixture;
    fixture.plan.mutations.push_back({});
    fixture.plan.events.insert(
        fixture.plan.events.end() - 2,
        GeneralPreflightPlan::ReplayEvent{
            .type = RenderTapeEventType::ResourceMutation,
            .eventIndex = 2u,
            .index = 0u});
    fixture.plan.events[3].eventIndex = 3u;
    fixture.plan.events[4].eventIndex = 4u;
    auto caps = GeneralCpuAdmissionCaps{};
    caps.maxMutations = 0u;
    const auto result = admitGeneralCpuPlan(fixture.plan, caps);
    check(result.reason == GeneralCpuAdmissionReason::MutationLimit &&
              result.eventIndex == 2u,
          "mutation bound reports its first mutation event");
  }
  {
    MiniPlan fixture;
    auto caps = GeneralCpuAdmissionCaps{};
    caps.maxChunks = 1u;
    const auto result = admitGeneralCpuPlan(fixture.plan, caps);
    check(result.reason == GeneralCpuAdmissionReason::ChunkLimit,
          "bootstrap and command chunks share the chunk bound");
  }
  {
    MiniPlan fixture;
    auto caps = GeneralCpuAdmissionCaps{};
    caps.maxRecords = 0u;
    const auto result = admitGeneralCpuPlan(fixture.plan, caps);
    check(result.reason == GeneralCpuAdmissionReason::RecordLimit &&
              result.chunkIndex == 0u,
          "aggregate record bound reports the bootstrap chunk");
  }
  {
    MiniPlan fixture;
    auto caps = GeneralCpuAdmissionCaps{};
    caps.maxRecordsPerChunk = 0u;
    const auto result = admitGeneralCpuPlan(fixture.plan, caps);
    check(result.reason == GeneralCpuAdmissionReason::PerChunkRecordLimit &&
              result.chunkIndex == 0u,
          "per-chunk record bound reports its chunk");
  }
  {
    MiniPlan fixture;
    fixture.plan.catalogue.blobs.push_back({.size = 2u});
    auto caps = GeneralCpuAdmissionCaps{};
    caps.maxBlobBytes = 1u;
    const auto result = admitGeneralCpuPlan(fixture.plan, caps);
    check(result.reason == GeneralCpuAdmissionReason::BlobBytesLimit,
          "blob byte bound is checked before effects");
  }
}

void grammarRejectsUnsupportedEventsAndControls() {
  {
    MiniPlan fixture;
    auto& gamma = fixture.plan.bootstrapGamma[0];
    gamma.resize(kRenderTapeGammaRampBytes);
    for (std::size_t index = 0u; index < 768u; ++index) {
      const auto value = static_cast<std::uint16_t>((index % 256u) << 8u);
      std::memcpy(gamma.data() + index * sizeof(value), &value, sizeof(value));
    }
    check(admit(fixture).accepted(), "identity bootstrap gamma is CPU state-only");
    gamma[256] ^= std::byte{1};
    check(admit(fixture).reason == GeneralCpuAdmissionReason::GammaUnsupported,
          "nonidentity bootstrap gamma stays outside the admitted lane");
  }
  {
    MiniPlan fixture;
    fixture.plan.bootstrapChunks[0] = fixture.plan.commandChunks[0];
    check(admit(fixture).reason == GeneralCpuAdmissionReason::UnsupportedCommandRecord,
          "bootstrap must not publish a command source before controlled draining");
  }
  {
    MiniPlan fixture;
    fixture.plan.events[0].type = static_cast<RenderTapeEventType>(99u);
    const auto result = admit(fixture);
    check(result.reason == GeneralCpuAdmissionReason::UnsupportedEvent &&
              result.eventIndex == 0u,
          "unknown event reports its event locator");
  }
  {
    MiniPlan fixture;
    fixture.plan.bootstrapGamma[0].push_back(std::byte{1});
    const auto result = admit(fixture);
    check(result.reason == GeneralCpuAdmissionReason::GammaUnsupported,
          "bootstrap gamma is outside the first general CPU grammar");
  }
  {
    MiniPlan fixture;
    fixture.plan.controls.push_back({});
    const auto result = admit(fixture);
    check(result.reason == GeneralCpuAdmissionReason::OrderedControlUnsupported,
          "ordered controls are outside the first general CPU grammar");
  }
  {
    MiniPlan fixture;
    fixture.plan.events[2].index = 4u;
    const auto result = admit(fixture);
    check(result.reason == GeneralCpuAdmissionReason::CommandChunkUnavailable &&
              result.eventIndex == 2u,
          "missing command chunk reports the command event");
  }
}

void commandGrammarReportsRecordLocator() {
  for (const auto type : {D9C_COMMAND_RECORD_READBACK,
                         D9C_COMMAND_RECORD_STRETCH_RECT,
                         D9C_COMMAND_RECORD_UPDATE_SURFACE}) {
    check(!detail::generalCpuCommandRecordSupported(type),
          "CPU attachment consumers reject before any resource effects");
  }
  MiniPlan fixture;
  D9CCommandChunkWireReadback readback{};
  fixture.presentBytes = test::singleRecordChunk(
      D9C_COMMAND_RECORD_READBACK,
      std::as_bytes(std::span(&readback, 1u)));
  fixture.plan.commandChunks[0].bytes = fixture.presentBytes;
  const auto result = admit(fixture);
  check(result.reason == GeneralCpuAdmissionReason::UnsupportedCommandRecord &&
            result.eventIndex == 2u && result.chunkIndex == 0u &&
            result.recordIndex == 0u,
        "unsupported readback record reports event/chunk/record locator");

  D9CCommandChunkWireQueryIssue constant{};
  fixture.presentBytes = test::singleRecordChunk(
      D9C_COMMAND_RECORD_SET_PS_CONST_I,
      std::as_bytes(std::span(&constant, 1u)));
  fixture.plan.commandChunks[0].bytes = fixture.presentBytes;
  const auto integerResult = admit(fixture);
  check(integerResult.reason == GeneralCpuAdmissionReason::UnsupportedCommandRecord,
        "integer constants remain outside the first general CPU grammar");
}

void queryGrammarRequiresVersionedTranscript() {
  MiniPlan fixture;
  const D9CWireObjectIdentity identity{
      .kind = D9C_CHUNK_HANDLE_KIND_QUERY, .generation = 2u, .objectId = 17u};
  const RenderTapeQueryDescriptorV2 query{.type = 8u, .dataBytes = 4u};
  fixture.plan.definitions.push_back({
      .fixed = {.identity = identity,
                .descriptorKind = static_cast<std::uint32_t>(RenderTapeDescriptorKind::Query),
                .descriptorBytes = sizeof(query)},
      .descriptor = std::as_bytes(std::span(&query, 1u))});
  const D9CCommandChunkWireQueryIssue issue{.queryHandleIndex = 0u, .flags = 1u};
  const D9CCommandChunkWireHandleEntry handle{
      .kind = identity.kind, .generation = identity.generation,
      .objectId = identity.objectId};
  const auto chunk = test::singleRecordChunk(D9C_COMMAND_RECORD_QUERY_ISSUE,
      std::as_bytes(std::span(&issue, 1u)), std::span(&handle, 1u));
  fixture.plan.commandChunks.push_back({
      .envelope = {.recordCount = 1u, .handleCount = 1u}, .bytes = chunk});
  RenderTapeQueryGetDataControlV2 poll{.issueCycle = 1u};
  fixture.plan.controls.push_back({
      .identity = identity,
      .kind = static_cast<std::uint32_t>(RenderTapeControlKind::QueryGetData),
      .disposition = static_cast<std::uint32_t>(RenderTapeControlDisposition::Completed),
      .controlBytes = sizeof(poll), .completionOrdinal = 1u});
  const auto pollBytes = std::as_bytes(std::span(&poll, 1u));
  fixture.plan.controlBytes.emplace_back(pollBytes.begin(), pollBytes.end());
  fixture.plan.events.insert(fixture.plan.events.begin() + 2u,
      {{.type = RenderTapeEventType::ObjectDefine, .index = 1u},
       {.type = RenderTapeEventType::CommandChunk, .index = 1u},
       {.type = RenderTapeEventType::OrderedControl, .index = 0u}});
  for (std::uint32_t event = 0u; event < fixture.plan.events.size(); ++event)
    fixture.plan.events[event].eventIndex = event;
  const auto admitted = admit(fixture);
  check(admitted.accepted() && admitted.counts.queryDefinitions == 1u &&
            admitted.counts.queryIssues == 1u && admitted.counts.queryPolls == 1u,
        "versioned query descriptor, canonical issue, and original poll are admitted together");

  const RenderTapeQueryDescriptor legacy{.type = 8u, .dataBytes = 4u};
  fixture.plan.definitions[1].descriptor = std::as_bytes(std::span(&legacy, 1u));
  fixture.plan.definitions[1].fixed.descriptorBytes = sizeof(legacy);
  const auto legacyDescriptor = admit(fixture);
  check(legacyDescriptor.reason == GeneralCpuAdmissionReason::QueryTranscriptUnavailable &&
            legacyDescriptor.eventIndex == 2u,
        "legacy query descriptor rejects with a named transcript reason before effects");
  fixture.plan.definitions[1].descriptor = std::as_bytes(std::span(&query, 1u));
  fixture.plan.definitions[1].fixed.descriptorBytes = sizeof(query);
  const RenderTapeQueryGetDataControl legacyPoll{};
  const auto legacyBytes = std::as_bytes(std::span(&legacyPoll, 1u));
  fixture.plan.controls[0].controlBytes = sizeof(legacyPoll);
  fixture.plan.controlBytes[0].assign(legacyBytes.begin(), legacyBytes.end());
  const auto legacyControl = admit(fixture);
  check(legacyControl.reason == GeneralCpuAdmissionReason::QueryTranscriptUnavailable &&
            legacyControl.eventIndex == 4u,
        "legacy query poll cannot silently join the qualified V2 transcript");
  poll.schemaVersion = 1u;
  fixture.plan.controls[0].controlBytes = sizeof(poll);
  fixture.plan.controlBytes[0].assign(pollBytes.begin(), pollBytes.end());
  check(admit(fixture).reason == GeneralCpuAdmissionReason::QueryTranscriptUnavailable,
        "wrong-version fixed-size poll also rejects before effects");
}

void presentAndOutputContractsAreEnforced() {
  {
    MiniPlan fixture;
    fixture.plan.events.pop_back();
    const auto result = admit(fixture);
    check(result.reason == GeneralCpuAdmissionReason::PresentCompleteCount,
          "missing final completion is rejected");
  }
  {
    MiniPlan fixture;
    fixture.plan.events.push_back(GeneralPreflightPlan::ReplayEvent{
        .type = RenderTapeEventType::PresentComplete, .eventIndex = 4u});
    const auto result = admit(fixture);
    check(result.reason == GeneralCpuAdmissionReason::PresentCompleteNotFinal,
          "events after completion are rejected");
  }
  {
    MiniPlan fixture;
    fixture.plan.outputIdentity = {};
    const auto result = admit(fixture);
    check(result.reason == GeneralCpuAdmissionReason::OutputDescriptorMissing,
          "output identity and descriptor are retained as an admission contract");
  }
  {
    MiniPlan fixture;
    auto secondPresent = test::presentChunk();
    fixture.plan.commandChunks.push_back({
        .envelope = CommandChunkEnvelope{
            .version = D9C_COMMAND_CHUNK_WIRE_VERSION,
            .recordCount = 1u,
            .handleCount = 0u},
        .bytes = secondPresent});
    fixture.plan.events.insert(
        fixture.plan.events.end() - 1,
        GeneralPreflightPlan::ReplayEvent{
            .type = RenderTapeEventType::CommandChunk,
            .eventIndex = 3u,
            .index = 1u});
    fixture.plan.events.back().eventIndex = 4u;
    const auto result = admit(fixture);
    check(result.reason == GeneralCpuAdmissionReason::PresentCount &&
              result.eventIndex == 3u && result.chunkIndex == 1u &&
              result.recordIndex == 0u,
          "a second Present reports its exact record locator");
  }
}

}  // namespace

int main() {
  try {
    acceptedPlanRetainsGeneralObjectsAndState();
    aggregateBoundsAreCheckedWithNamedReasons();
    grammarRejectsUnsupportedEventsAndControls();
    commandGrammarReportsRecordLocator();
    queryGrammarRequiresVersionedTranscript();
    presentAndOutputContractsAreEnforced();
  } catch (const TestFailure& failure) {
    std::cerr << "render_tape_cpu_general_grammar_spec: " << failure.what()
              << '\n';
    return 1;
  }
  return 0;
}
