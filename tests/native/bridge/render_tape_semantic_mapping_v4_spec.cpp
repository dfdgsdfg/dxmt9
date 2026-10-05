#include "device_c_render_tape_semantic_mapping_v4.hpp"
#include "render_tape_identity_v4_fixture.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <new>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace semantic_mapping_allocation_probe {
bool enabled = false;
std::size_t failAfter = 0u;
} // namespace semantic_mapping_allocation_probe

void* operator new(std::size_t size) {
  if (semantic_mapping_allocation_probe::enabled) {
    if (!semantic_mapping_allocation_probe::failAfter) {
      semantic_mapping_allocation_probe::enabled = false;
      throw std::bad_alloc();
    }
    --semantic_mapping_allocation_probe::failAfter;
  }
  if (auto* value = std::malloc(size ? size : 1u)) return value;
  throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* value) noexcept { std::free(value); }
void operator delete[](void* value) noexcept { ::operator delete(value); }
void operator delete(void* value, std::size_t) noexcept { ::operator delete(value); }
void operator delete[](void* value, std::size_t) noexcept { ::operator delete(value); }

namespace {

using namespace dxmt9::d3d9;
using Status = RenderTapeSemanticMappingV4Status;
using Kind = RenderTapeIdentityV3DispositionKind;
using Command = RenderTapeIdentityV3CommandKind;
using Cut = RenderTapeIdentityV3CutKind;
using Prefix = RenderTapeIdentityV3PrefixKind;
constexpr auto noIndex = kRenderTapeIdentityV4NoIndex;
constexpr D9CWireObjectIdentity output{D9C_CHUNK_HANDLE_KIND_SURFACE, 1u, 51u};
constexpr D9CWireObjectIdentity query{D9C_CHUNK_HANDLE_KIND_QUERY, 3u, 42u};
std::uint64_t checks = 0u;
std::uint64_t partitionPairs = 0u, prefixPerturbations = 0u;

void check(bool value, std::string_view message) {
  ++checks;
  if (!value) throw std::runtime_error(std::string(message));
}

std::vector<std::byte> mixedChunk(std::uint32_t draws = 2u) {
  const D9CCommandChunkWireDrawHeader draw{
      .primitiveType = 4u, .primitiveCount = 1u,
      .sectionTableOffset = sizeof(D9CCommandChunkWireDrawHeader),
      .sectionPayloadOffset = sizeof(D9CCommandChunkWireDrawHeader)};
  const D9CCommandChunkWireSetConst constants{.registerCount = 1u};
  std::vector<std::byte> constantBytes(sizeof(constants) + 4u * sizeof(float));
  std::memcpy(constantBytes.data(), &constants, sizeof(constants));
  const auto drawBytes = std::as_bytes(std::span(&draw, 1u));
  const auto recordCount = draws * 2u - 1u;
  const auto recordOffset = sizeof(D9CCommandChunkWireHeader);
  const auto payloadOffset = test::alignUp(recordOffset +
      recordCount * sizeof(D9CCommandChunkWireRecordHeader), 8u);
  std::vector<D9CCommandChunkWireRecordHeader> records(recordCount);
  std::vector<std::byte> payload;
  for (std::size_t index = 0u; index < records.size(); ++index) {
    const auto bytes = index % 2u ? std::span<const std::byte>(constantBytes) : drawBytes;
    payload.resize(test::alignUp(payload.size(), 4u));
    records[index] = {.type = index % 2u ? D9C_COMMAND_RECORD_SET_VS_CONST_F : D9C_COMMAND_RECORD_DRAW_PRIMITIVE,
        .payloadOffset = static_cast<std::uint32_t>(payload.size()),
        .payloadSize = static_cast<std::uint32_t>(bytes.size())};
    payload.insert(payload.end(), bytes.begin(), bytes.end());
  }
  const D9CCommandChunkWireHeader header{
      .version = D9C_COMMAND_CHUNK_WIRE_VERSION, .headerSize = D9C_COMMAND_CHUNK_WIRE_HEADER_SIZE,
      .recordHeaderSize = D9C_COMMAND_CHUNK_WIRE_RECORD_HEADER_SIZE,
      .handleEntrySize = D9C_COMMAND_CHUNK_WIRE_HANDLE_ENTRY_SIZE,
      .recordTableOffset = static_cast<std::uint32_t>(recordOffset), .recordCount = recordCount,
      .handleTableOffset = static_cast<std::uint32_t>(payloadOffset),
      .payloadArenaOffset = static_cast<std::uint32_t>(payloadOffset),
      .payloadArenaSize = static_cast<std::uint32_t>(payload.size())};
  std::vector<std::byte> bytes(payloadOffset + payload.size());
  std::memcpy(bytes.data(), &header, sizeof(header));
  std::memcpy(bytes.data() + recordOffset, records.data(), records.size() * sizeof(records[0]));
  std::memcpy(bytes.data() + payloadOffset, payload.data(), payload.size());
  return bytes;
}

// The same authentic raw/record stream admits different observed publication
// layouts. State remains between the two contributors to one merged DrawRun;
// delayed publication happens after END without changing its immutable cursor.
test::IdentityV4Fixture mixedFixture(bool split, bool delayed = false) {
  test::IdentityV4Fixture fixture;
  RenderTapeBuilder builder;
  builder.appendBootstrapState(test::fullStateChunk(false));
  const RenderTapeSurfaceDescriptorV2 surface{
      .storage = static_cast<std::uint32_t>(RenderTapeSurfaceStorage::SwapchainBackbuffer),
      .initialContentDisposition = static_cast<std::uint32_t>(
          RenderTapeInitialContentDisposition::ProducedPresentOutput),
      .surface = {.format = 21u, .resourceType = 1u, .usage = 1u,
          .width = 4u, .height = 4u, .depth = 1u}};
  builder.appendObjectDefine(output, static_cast<std::uint32_t>(RenderTapeDescriptorKind::Surface),
      std::as_bytes(std::span(&surface, 1u)), 0u, {});
  const RenderTapeQueryDescriptorV2 descriptor{.type = 8u, .dataBytes = 4u};
  builder.appendObjectDefine(query, static_cast<std::uint32_t>(RenderTapeDescriptorKind::Query),
      std::as_bytes(std::span(&descriptor, 1u)), 0u, {});
  builder.appendCommandChunk({.recordCount = 3u}, mixedChunk());
  const D9CCommandChunkWireQueryIssue issue{.flags = 1u};
  const D9CCommandChunkWireHandleEntry handle{
      .kind = query.kind, .generation = query.generation, .objectId = query.objectId};
  builder.appendCommandChunk({.recordCount = 1u, .handleCount = 1u}, test::singleRecordChunk(
      D9C_COMMAND_RECORD_QUERY_ISSUE, std::as_bytes(std::span(&issue, 1u)), std::span(&handle, 1u)));
  builder.appendCommandChunk({.recordCount = 1u}, test::fullStateChunk(true));
  builder.appendCommandChunk({.recordCount = 1u}, test::presentChunk());
  const RenderTapeOracleAttachment oracle{.identity = output,
      .descriptorKind = static_cast<std::uint32_t>(RenderTapeDescriptorKind::Surface)};
  builder.appendPresentComplete(7u, 1u, RenderTapeDigestValidity::NotCaptured, {},
      std::as_bytes(std::span(&oracle, 1u)));
  fixture.tape = builder.seal();
  auto& identity = fixture.identity;
  identity.header.frameId = 1u; identity.header.presentOrdinal = 7u;
  identity.header.captureToken = 7u; identity.header.captureEpoch = 8u;
  identity.header.queue = 9u; identity.header.initializerEpoch = 9u;
  identity.header.baselineCompletionEdge = 1u;
  identity.header.baselineKind = static_cast<std::uint32_t>(Prefix::EmptyBaseline);
  identity.header.coordinatorDomain = static_cast<std::uint32_t>(TapeCoordinatorAuthority::OrdinarySerial);
  const std::uint32_t preEndCommands = split ? 2u : 1u;
  for (std::uint32_t source = 0u; source <= preEndCommands; ++source) {
    const bool later = source == preEndCommands;
    identity.sources.push_back({.lifecycle = {.sourceOrdinal = source + 1u, .seqId = source + 1u,
        .writerCaptureId = source + 1u, .writerGeneration = 2u, .storageGeneration = 3u,
        .publicationEdge = later ? 350u : split ? 20u + source * 20u : delayed ? 194u : 94u,
        .submissionEdge = 500u + source * 2u, .completionEdge = 501u + source * 2u,
        .completedSequence = source + 1u,
        .cutEventOrdinal = later ? 7u : split ? 4u : delayed ? 6u : 5u,
        .commandCount = later ? 2u : 1u,
        .cutKind = static_cast<std::uint32_t>(later || split ? Cut::AfterRecord : Cut::BeforeEvent),
        .cutRecord = later ? 1u : split ? 1u + source * 2u : 0u}});
  }
  for (std::uint32_t command = 0u; command < preEndCommands; ++command)
    identity.commands.push_back({.sourceIndex = command,
        .kind = static_cast<std::uint32_t>(Command::DrawRun), .drawCount = split ? 1u : 2u});
  identity.commands.push_back({.sourceIndex = preEndCommands,
      .kind = static_cast<std::uint32_t>(Command::DrawRun), .drawCount = 1u});
  identity.commands.push_back({.sourceIndex = preEndCommands, .physicalCommandIndex = 1u,
      .kind = static_cast<std::uint32_t>(Command::Present)});
  identity.dispositions = {{.eventOrdinal = 4u, .recordCount = 1u,
      .kind = static_cast<std::uint32_t>(Kind::GpuCommand), .commandIndex = 0u, .subdrawCount = 1u},
      {.eventOrdinal = 4u, .firstRecord = 1u, .recordCount = 1u,
          .kind = static_cast<std::uint32_t>(Kind::StateOnly)},
      {.eventOrdinal = 4u, .firstRecord = 2u, .recordCount = 1u,
          .kind = static_cast<std::uint32_t>(Kind::GpuCommand), .commandIndex = split ? 1u : 0u,
          .firstSubdraw = split ? 0u : 1u, .subdrawCount = 1u},
      {.eventOrdinal = 5u, .recordCount = 1u,
          .kind = static_cast<std::uint32_t>(Kind::Coordinator), .coordinatorIndex = 0u},
      {.eventOrdinal = 6u, .recordCount = 1u,
          .kind = static_cast<std::uint32_t>(Kind::GpuCommand),
          .commandIndex = preEndCommands, .subdrawCount = 1u},
      {.eventOrdinal = 7u, .recordCount = 1u,
          .kind = static_cast<std::uint32_t>(Kind::GpuCommand), .commandIndex = preEndCommands + 1u}};
  identity.coordinators = {{.provenance = {.eventOrdinal = 5u, .rawOrdinal = 5u,
      .dispatchEdge = 150u, .queryIdentity = query, .issueCycle = 1u, .issueOrdinal = 1u,
      .deviceEpoch = 77u, .writerCaptureId = preEndCommands, .writerGeneration = 2u,
      .storageGeneration = 3u, .prefixSequence = preEndCommands,
      .recordType = D9C_COMMAND_RECORD_QUERY_ISSUE, .flags = 1u, .queryType = 8u,
      .prefixKind = static_cast<std::uint32_t>(Prefix::RealSource),
      .sourceIndex = preEndCommands - 1u, .commandEnd = 1u},
      .admissionEdge = 149u, .authority = static_cast<std::uint32_t>(TapeCoordinatorAuthority::OrdinarySerial)}};
  identity.settlements = {{4u, 4u, 91u, 3u}, {5u, 5u, 191u, 1u},
      {6u, 6u, 291u, 1u}, {7u, 7u, 391u, 1u}};
  test::setWholeRawCompatibilityAttempts(identity);
  auto& topology = fixture.topology;
  topology.header.captureToken = 7u; topology.header.captureEpoch = 8u; topology.header.queue = 9u;
  topology.plans = {{.planId = 1u, .acceptanceEdge = 450u,
      .route = static_cast<std::uint32_t>(RenderTapeExecutionTopologyV1Route::Optimized),
      .placementCount = static_cast<std::uint32_t>(identity.commands.size()),
      .executedCount = static_cast<std::uint32_t>(identity.commands.size())}};
  for (std::uint32_t command = 0u; command < identity.commands.size(); ++command) {
    const bool present = command + 1u == identity.commands.size();
    topology.placements.push_back({.planIndex = 0u, .commandIndex = command,
        .outcome = static_cast<std::uint32_t>(present
            ? RenderTapeExecutionTopologyV1Outcome::ExecutedWithoutDag
            : RenderTapeExecutionTopologyV1Outcome::ExecutedDagPass),
        .executionOrdinal = command, .dagPassIndex = present ? noIndex : 17u,
        .passKind = present ? 0u : 1u});
  }
  return fixture;
}

// Each gap has three legal states: continue this DrawRun, start a new command
// in this source, or start a new command/source. Thus all 3^(draws-1) physical
// partitions are enumerated without constructing an illegal source split
// through a DrawRun. Every draw pair still has a genuine intervening state row.
test::IdentityV4Fixture partitionFixture(std::uint32_t draws, std::uint32_t mask) {
  auto fixture = mixedFixture(false);
  RenderTapeBuilder builder;
  builder.appendBootstrapState(test::fullStateChunk(false));
  const RenderTapeSurfaceDescriptorV2 surface{
      .storage = static_cast<std::uint32_t>(RenderTapeSurfaceStorage::SwapchainBackbuffer),
      .initialContentDisposition = static_cast<std::uint32_t>(
          RenderTapeInitialContentDisposition::ProducedPresentOutput),
      .surface = {.format = 21u, .resourceType = 1u, .usage = 1u,
          .width = 4u, .height = 4u, .depth = 1u}};
  builder.appendObjectDefine(output, static_cast<std::uint32_t>(RenderTapeDescriptorKind::Surface),
      std::as_bytes(std::span(&surface, 1u)), 0u, {});
  const RenderTapeQueryDescriptorV2 descriptor{.type = 8u, .dataBytes = 4u};
  builder.appendObjectDefine(query, static_cast<std::uint32_t>(RenderTapeDescriptorKind::Query),
      std::as_bytes(std::span(&descriptor, 1u)), 0u, {});
  builder.appendCommandChunk({.recordCount = draws * 2u - 1u}, mixedChunk(draws));
  const D9CCommandChunkWireQueryIssue issue{.flags = 1u};
  const D9CCommandChunkWireHandleEntry handle{
      .kind = query.kind, .generation = query.generation, .objectId = query.objectId};
  builder.appendCommandChunk({.recordCount = 1u, .handleCount = 1u}, test::singleRecordChunk(
      D9C_COMMAND_RECORD_QUERY_ISSUE, std::as_bytes(std::span(&issue, 1u)), std::span(&handle, 1u)));
  builder.appendCommandChunk({.recordCount = 1u}, test::fullStateChunk(true));
  builder.appendCommandChunk({.recordCount = 1u}, test::presentChunk());
  const RenderTapeOracleAttachment oracle{.identity = output,
      .descriptorKind = static_cast<std::uint32_t>(RenderTapeDescriptorKind::Surface)};
  builder.appendPresentComplete(7u, 1u, RenderTapeDigestValidity::NotCaptured, {},
      std::as_bytes(std::span(&oracle, 1u)));
  fixture.tape = builder.seal();
  auto& identity = fixture.identity;
  identity.sources.clear(); identity.commands.clear(); identity.dispositions.clear();
  std::vector<std::uint32_t> sourceCommands{0u}, sourceLastRecord{0u};
  std::uint32_t source = 0u;
  for (std::uint32_t draw = 0u; draw < draws; ++draw) {
    const auto gap = draw ? mask % 3u : 1u;
    if (draw) mask /= 3u;
    if (gap == 2u) {
      ++source; sourceCommands.push_back(0u); sourceLastRecord.push_back(0u);
    }
    if (gap) {
      identity.commands.push_back({.sourceIndex = source,
          .physicalCommandIndex = sourceCommands[source]++,
          .kind = static_cast<std::uint32_t>(Command::DrawRun)});
    }
    auto& command = identity.commands.back();
    identity.dispositions.push_back({.eventOrdinal = 4u, .firstRecord = draw * 2u, .recordCount = 1u,
        .kind = static_cast<std::uint32_t>(Kind::GpuCommand),
        .commandIndex = static_cast<std::uint32_t>(identity.commands.size() - 1u),
        .firstSubdraw = command.drawCount++, .subdrawCount = 1u});
    sourceLastRecord[source] = draw * 2u;
    if (draw + 1u < draws)
      identity.dispositions.push_back({.eventOrdinal = 4u, .firstRecord = draw * 2u + 1u,
          .recordCount = 1u, .kind = static_cast<std::uint32_t>(Kind::StateOnly)});
  }
  const auto preEndSources = source + 1u;
  for (std::uint32_t index = 0u; index <= preEndSources; ++index) {
    const bool later = index == preEndSources;
    const bool last = index + 1u == preEndSources;
    identity.sources.push_back({.lifecycle = {.sourceOrdinal = index + 1u, .seqId = index + 1u,
        .writerCaptureId = index + 1u, .writerGeneration = 2u, .storageGeneration = 3u,
        .publicationEdge = later ? 350u : last ? 94u : 20u + index * 10u,
        .submissionEdge = 500u + index * 2u, .completionEdge = 501u + index * 2u,
        .completedSequence = index + 1u, .cutEventOrdinal = later ? 7u : last ? 5u : 4u,
        .commandCount = later ? 2u : sourceCommands[index],
        .cutKind = static_cast<std::uint32_t>(last ? Cut::BeforeEvent : Cut::AfterRecord),
        .cutRecord = later ? 1u : last ? 0u : sourceLastRecord[index] + 1u}});
  }
  const auto youngerCommand = static_cast<std::uint32_t>(identity.commands.size());
  identity.commands.push_back({.sourceIndex = preEndSources,
      .kind = static_cast<std::uint32_t>(Command::DrawRun), .drawCount = 1u});
  identity.commands.push_back({.sourceIndex = preEndSources, .physicalCommandIndex = 1u,
      .kind = static_cast<std::uint32_t>(Command::Present)});
  identity.dispositions.push_back({.eventOrdinal = 5u, .recordCount = 1u,
      .kind = static_cast<std::uint32_t>(Kind::Coordinator), .coordinatorIndex = 0u});
  identity.dispositions.push_back({.eventOrdinal = 6u, .recordCount = 1u,
      .kind = static_cast<std::uint32_t>(Kind::GpuCommand), .commandIndex = youngerCommand, .subdrawCount = 1u});
  identity.dispositions.push_back({.eventOrdinal = 7u, .recordCount = 1u,
      .kind = static_cast<std::uint32_t>(Kind::GpuCommand), .commandIndex = youngerCommand + 1u});
  auto& end = identity.coordinators[0].provenance;
  end.writerCaptureId = end.prefixSequence = preEndSources;
  end.sourceIndex = preEndSources - 1u; end.commandEnd = sourceCommands.back();
  identity.settlements[0].recordCount = draws * 2u - 1u;
  test::setWholeRawCompatibilityAttempts(identity);
  fixture.topology.plans[0].placementCount = fixture.topology.plans[0].executedCount =
      static_cast<std::uint32_t>(identity.commands.size());
  fixture.topology.placements.clear();
  for (std::uint32_t command = 0u; command < identity.commands.size(); ++command) {
    const bool present = command + 1u == identity.commands.size();
    fixture.topology.placements.push_back({.planIndex = 0u, .commandIndex = command,
        .outcome = static_cast<std::uint32_t>(present
            ? RenderTapeExecutionTopologyV1Outcome::ExecutedWithoutDag
            : RenderTapeExecutionTopologyV1Outcome::ExecutedDagPass),
        .executionOrdinal = command, .dagPassIndex = present ? noIndex : 17u,
        .passKind = present ? 0u : 1u});
  }
  return fixture;
}

CaptureSourceReplayPlanV4 admit(const test::IdentityV4Fixture& fixture) {
  RenderTapeIdentityV4Status identityStatus{};
  const auto identity = buildRenderTapeIdentityV4(fixture.tape, {}, fixture.identity, &identityStatus);
  check(identityStatus == RenderTapeIdentityV4Status::Valid, "fixture has genuine canonical V4 provenance");
  RenderTapeExecutionTopologyV1Status topologyStatus{};
  const auto topology = buildRenderTapeExecutionTopologyV1(fixture.tape, {}, identity,
      fixture.topology, &topologyStatus);
  check(topologyStatus == RenderTapeExecutionTopologyV1Status::Valid, "original has a genuine topology companion");
  CaptureSourceReplayPlanV4 plan;
  check(buildCaptureSourceReplayPlanV4(fixture.tape, {}, identity, topology, plan) ==
      CaptureSourceReplayPlanV4Status::Valid, "full pair admission owns immutable original semantic facts");
  return plan;
}

struct Input {
  std::vector<RenderTapeIdentityV3EventFact> events;
  std::vector<RenderTapeIdentityV3RecordFact> localRecords;
  std::vector<CaptureSourceReplayPlanV4Action> obligations;
};

Input input(const CaptureSourceReplayPlanV4& plan) {
  Input values;
  for (const auto& event : plan.events())
    values.events.push_back({event.eventOrdinal, event.type, event.recordCount});
  values.localRecords.assign(plan.records().begin(), plan.records().end());
  for (const auto& action : plan.actions())
    if (action.kind == CaptureSourceReplayPlanV4ActionKind::ExpectSourcePublication)
      values.obligations.push_back(action);
  return values;
}

Status build(const CaptureSourceReplayPlanV4& plan, const RenderTapeIdentityV4View& candidate,
    const Input& values, RenderTapeSemanticMappingV4& mapping,
    RenderTapeSemanticMappingV4Limits limits = {}) {
  return buildRenderTapeSemanticMappingV4(plan, candidate, values.events, values.localRecords,
      values.obligations, limits, mapping);
}

bool empty(const RenderTapeSemanticMappingV4& mapping) {
  return !mapping.valid() && mapping.records().empty() && mapping.commands().empty() &&
      mapping.sources().empty() && mapping.raws().empty() && mapping.coordinators().empty() &&
      mapping.obligations().empty() && mapping.planDigest() == RenderTapeDigest{} &&
      mapping.candidateCapture() == RenderTapeIdentityV4CaptureKey{} && !mapping.candidateQueue();
}

void regroupedDrawsAndDelayedPublication() {
  for (const bool splitOriginal : {false, true}) {
    auto original = mixedFixture(splitOriginal);
    auto candidate = mixedFixture(!splitOriginal, splitOriginal);
    auto plan = admit(original);
    const auto values = input(plan);
    RenderTapeSemanticMappingV4 mapping;
    check(build(plan, candidate.identity, values, mapping) == Status::Valid,
        "authentic split/merged DrawRuns map without source/count/cut equality");
    check(mapping.records().size() == 6u && mapping.raws().size() == 4u &&
        mapping.commands().size() == 4u && mapping.sources().size() == 3u &&
        mapping.obligations().size() == original.identity.sources.size(),
        "direct command and many-to-many source correspondence account for all real origins");
    const auto& first = mapping.records()[0];
    const auto& state = mapping.records()[1];
    const auto& second = mapping.records()[2];
    check(first.recordIndex == 0u && state.recordIndex == 1u && second.recordIndex == 2u &&
        state.dispositionKind == static_cast<std::uint32_t>(Kind::StateOnly) &&
        state.candidateCommandIndex == noIndex && state.capturedCommandIndex == noIndex &&
        state.candidateSubdraw == noIndex &&
        first.capturedCommandIndex == 0u && second.capturedCommandIndex == (splitOriginal ? 1u : 0u) &&
        second.capturedSubdraw == (splitOriginal ? 0u : 1u) &&
        second.candidateSubdraw == (splitOriginal ? 1u : 0u),
        "nonadjacent draw contributors preserve immutable original subdraws and state order");
    const auto& end = mapping.coordinators()[0];
    check(end.prefixRecordCount == 2u && end.prefixCoordinatorCommandCount == 0u &&
        end.candidateSourceIndex == candidate.identity.coordinators[0].provenance.sourceIndex &&
        end.candidateCommandEnd == 1u && mapping.records()[4].eventOrdinal == 6u,
        "END normalization includes prior sources and excludes the younger draw");
    for (const auto& obligation : mapping.obligations()) {
      check(obligation.sourceRelationCount > 0u &&
          obligation.firstSourceRelation + obligation.sourceRelationCount <= mapping.sources().size(),
          "each deferred original publication discharges actual nonempty source membership");
      for (const auto& source : mapping.sources().subspan(
              obligation.firstSourceRelation, obligation.sourceRelationCount))
        check(source.capturedIndex == obligation.capturedIndex && source.candidateSequence &&
            source.candidatePublicationEdge, "obligation range contains only authenticated candidate contributors");
    }
    const auto digest = mapping.planDigest();
    auto owned = std::move(mapping);
    check(empty(mapping) && owned.valid() && owned.planDigest() == digest,
        "move invalidates the source while retaining owned output");
    auto* sameOwner = &owned;
    owned = std::move(*sameOwner);
    check(owned.valid() && owned.records().size() == 6u, "self-move preserves a complete mapping");
    candidate.identity = {}; original = {}; plan = {};
    check(owned.valid() && owned.records()[2].recordIndex == 2u && owned.planDigest() == digest,
        "mapping owns all locators after caller facts and identities are gone");
  }
}

void queryTranslationsAndIndependentBaselines() {
  auto fixture = mixedFixture(true);
  auto plan = admit(fixture);
  auto values = input(plan);
  auto candidate = mixedFixture(false, true).identity;
  candidate.header.captureToken = 70u; candidate.header.captureEpoch = 80u; candidate.header.queue = 90u;
  candidate.header.baselineKind = static_cast<std::uint32_t>(Prefix::ArmBaseline);
  candidate.header.baselineSequence = candidate.header.baselineCompletedSequence = 100u;
  candidate.header.baselineInitializer = candidate.header.baselineCompletedInitializer = 10u;
  for (auto& source : candidate.sources) {
    source.lifecycle.seqId += 100u; source.lifecycle.completedSequence += 100u;
  }
  for (auto& settlement : candidate.settlements) settlement.rawOrdinal += 100u;
  for (auto& attempt : candidate.attempts) {
    attempt.rawOrdinal += 100u; attempt.identity.sequence += 100u;
    attempt.receipt.identity.sequence += 100u; attempt.receipt.queueSequence += 100u;
  }
  auto& coordinator = candidate.coordinators[0].provenance;
  coordinator.rawOrdinal += 100u; coordinator.prefixSequence += 100u;
  coordinator.prefixInitializer = 10u; coordinator.prefixInitializerEpoch = 9u;
  coordinator.queryIdentity = {D9C_CHUNK_HANDLE_KIND_QUERY, 4u, 123u};
  coordinator.issueCycle = 3u; coordinator.issueOrdinal = 7u; coordinator.deviceEpoch = 999u;
  values.localRecords[3].queryIdentity = coordinator.queryIdentity;
  values.localRecords[3].issueCycle = coordinator.issueCycle;
  values.localRecords[3].issueOrdinal = coordinator.issueOrdinal;
  RenderTapeSemanticMappingV4 mapping;
  check(build(plan, candidate, values, mapping) == Status::Valid &&
      mapping.candidateCapture() == RenderTapeIdentityV4CaptureKey{70u, 80u} &&
      mapping.candidateQueue() == 90u && mapping.records()[0].capturedRawOrdinal == 4u &&
      mapping.records()[0].candidateRawOrdinal == 104u &&
      mapping.coordinators()[0].candidatePrefixInitializer == 10u,
      "local query/raw identity and completed arm dependencies translate independently of captured IDs");
  candidate.header.baselineCompletedInitializer = 9u;
  check(build(plan, candidate, values, mapping) == Status::InvalidCandidate && empty(mapping),
      "baseline dependency must actually be locally completed even when source IDs correspond");
  candidate.header.baselineCompletedInitializer = 10u;
  candidate.coordinators[0].provenance.queryIdentity.objectId++;
  check(build(plan, candidate, values, mapping) == Status::InvalidCandidate && empty(mapping),
      "candidate query identity must match supplied canonical-local facts");
  candidate.coordinators[0].provenance.queryIdentity.objectId--;
  values.localRecords[3].queryFlags = 2u;
  check(build(plan, candidate, values, mapping) == Status::InvalidFacts && empty(mapping),
      "caller local facts cannot relabel original END flags");

  auto emptyFixture = test::makeIdentityV4CoordinatorFixture(TapeCoordinatorAuthority::OrdinarySerial);
  auto emptyPlan = admit(emptyFixture);
  auto emptyValues = input(emptyPlan);
  auto armBaseline = emptyFixture.identity;
  armBaseline.header.baselineKind = static_cast<std::uint32_t>(Prefix::ArmBaseline);
  armBaseline.header.baselineSequence = armBaseline.header.baselineCompletedSequence = 10u;
  armBaseline.sources[0].lifecycle.seqId = armBaseline.sources[0].lifecycle.completedSequence = 11u;
  armBaseline.coordinators[1].provenance.prefixKind = static_cast<std::uint32_t>(Prefix::ArmBaseline);
  armBaseline.coordinators[1].provenance.prefixSequence = 10u;
  for (std::size_t index = 0u; index < 2u; ++index) {
    auto& control = armBaseline.coordinators[index].provenance;
    control.queryIdentity = {D9C_CHUNK_HANDLE_KIND_QUERY, 4u, 123u};
    control.issueCycle += 2u; control.issueOrdinal += 6u;
    emptyValues.localRecords[index].queryIdentity = control.queryIdentity;
    emptyValues.localRecords[index].issueCycle = control.issueCycle;
    emptyValues.localRecords[index].issueOrdinal = control.issueOrdinal;
  }
  check(build(emptyPlan, armBaseline, emptyValues, mapping) == Status::Valid &&
      mapping.coordinators()[1].prefixRecordCount == 0u &&
      mapping.coordinators()[1].candidatePrefixKind == static_cast<std::uint32_t>(Prefix::ArmBaseline) &&
      mapping.coordinators()[1].candidatePrefixSequence == 10u,
      "genuine Empty-to-Arm correspondence preserves local completed baseline authority");
  ++armBaseline.coordinators[1].provenance.issueCycle;
  ++emptyValues.localRecords[1].issueCycle;
  check(build(emptyPlan, armBaseline, emptyValues, mapping) == Status::InvalidFacts && empty(mapping),
      "local facts cannot invent a new issue cycle between canonical BEGIN and END");
}

void malformedCoverageAndPrefixes() {
  auto fixture = mixedFixture(true);
  auto plan = admit(fixture);
  const auto values = input(plan);
  const auto good = mixedFixture(false, true).identity;
  RenderTapeSemanticMappingV4 mapping;
  const auto rejected = [&](RenderTapeIdentityV4View candidate, Status expected, std::string_view message) {
    check(build(plan, good, values, mapping) == Status::Valid, "reseed complete output before rejecting replacement");
    check(build(plan, candidate, values, mapping) == expected && empty(mapping), message);
  };
  auto wrong = good;
  wrong.dispositions[2].firstSubdraw = 0u;
  rejected(wrong, Status::InvalidCandidate, "duplicate original subdraw coverage rejects atomically");
  wrong = good; wrong.dispositions[1].firstRecord = 2u;
  rejected(wrong, Status::InvalidCandidate, "wrong canonical record location rejects atomically");
  wrong = good; wrong.settlements[0].rawOrdinal++;
  rejected(wrong, Status::InvalidCandidate, "ACK and committed attempt must name the same actual raw");
  wrong = good; wrong.header.presentOrdinal++;
  rejected(wrong, Status::SemanticMismatch, "candidate Present ordinal stays bound to the admitted interval");
  wrong = good; wrong.dispositions.erase(wrong.dispositions.begin() + 1u);
  rejected(wrong, Status::InvalidCandidate, "omitted state record cannot disappear behind a merged DrawRun");
  wrong = good; wrong.dispositions.insert(wrong.dispositions.begin() + 1u, wrong.dispositions[0]);
  rejected(wrong, Status::InvalidCandidate, "duplicate disposition coverage cannot be mapped twice");
  wrong = good; wrong.sources[0].lifecycle.completionEdge = 0u;
  wrong.sources[0].lifecycle.completedSequence = 0u;
  rejected(wrong, Status::InvalidCandidate, "publication does not fabricate source GPU completion");
  wrong = good;
  auto& omitted = wrong.coordinators[0].provenance;
  omitted.prefixKind = static_cast<std::uint32_t>(Prefix::EmptyBaseline);
  omitted.writerCaptureId = omitted.writerGeneration = omitted.storageGeneration = 0u;
  omitted.prefixSequence = 0u; omitted.sourceIndex = noIndex; omitted.commandEnd = 0u;
  check(validateRenderTapeIdentityV4Rows(wrong, values.events, values.localRecords, true) ==
      RenderTapeIdentityV4Status::Valid, "baseline omission distinguishes correspondence from general shape admission");
  rejected(wrong, Status::InvalidPrefix, "baseline END cannot omit genuine pre-END GPU origins");
  wrong = good;
  auto& younger = wrong.coordinators[0].provenance;
  younger.sourceIndex = 1u; younger.prefixSequence = 2u; younger.writerCaptureId = 2u;
  younger.commandEnd = 1u;
  rejected(wrong, Status::InvalidCandidate, "END cannot include the younger draw by extending the real source prefix");
  auto badFacts = values; badFacts.localRecords[1].recordType = D9C_COMMAND_RECORD_DRAW_PRIMITIVE;
  check(build(plan, good, badFacts, mapping) == Status::InvalidFacts && empty(mapping),
      "candidate-supplied facts cannot relabel authentic state as a draw");
  badFacts = values; badFacts.events[3].recordCount++;
  check(build(plan, good, badFacts, mapping) == Status::InvalidFacts && empty(mapping),
      "event cardinality stays bound to the admitted canonical tape");
  badFacts = values; badFacts.obligations.erase(badFacts.obligations.begin());
  check(build(plan, good, badFacts, mapping) == Status::SemanticMismatch && empty(mapping),
      "every deferred original source action must be registered exactly once");
  badFacts = values; badFacts.obligations[0].tableIndex = badFacts.obligations[1].tableIndex;
  check(build(plan, good, badFacts, mapping) == Status::SemanticMismatch && empty(mapping),
      "equal obligation counts cannot substitute wrong original source membership");
  badFacts = values; std::swap(badFacts.obligations[0], badFacts.obligations[1]);
  check(build(plan, good, badFacts, mapping) == Status::SemanticMismatch && empty(mapping),
      "deferred actions retain authenticated original publication order");
}

void realRollbackAndRetry() {
  auto fixture = mixedFixture(false);
  auto plan = admit(fixture);
  const auto values = input(plan);
  auto candidate = mixedFixture(true).identity;
  auto prefix = test::compatibilityAttempt(4u, 4u, 4u, 0u, 1u, 2u, 10u);
  auto rollback = test::compatibilityAttempt(4u, 4u, 5u, 1u, 2u, 11u, 14u);
  rollback.outcome = static_cast<std::uint32_t>(RenderTapeIdentityV4AttemptOutcome::RolledBack);
  rollback.receipt = {};
  auto suffix = test::compatibilityAttempt(4u, 4u, 6u, 1u, 2u, 15u, 90u);
  candidate.attempts.erase(candidate.attempts.begin());
  for (auto& attempt : candidate.attempts) attempt.attemptGeneration += 2u;
  candidate.coordinators[0].provenance.buildGeneration += 2u;
  candidate.attempts.insert(candidate.attempts.begin(), {prefix, rollback, suffix});
  RenderTapeSemanticMappingV4 mapping;
  check(build(plan, candidate, values, mapping) == Status::Valid &&
      mapping.raws()[0].committedAttemptCount == 2u && mapping.raws()[0].rolledBackAttemptCount == 1u &&
      mapping.records().size() == plan.records().size(),
      "authentic prefix commit, pre-effect rollback and suffix retry map each semantic record once");
  candidate.attempts[2].firstRecord = 0u;
  check(build(plan, candidate, values, mapping) == Status::InvalidCandidate && empty(mapping),
      "retry cannot reconsume the already committed prefix");
  candidate.attempts[2].firstRecord = 1u; candidate.attempts[1].effectEdge = 12u;
  check(build(plan, candidate, values, mapping) == Status::InvalidCandidate && empty(mapping),
      "rollback with an irreversible effect cannot manufacture recovery");
}

void initializerAndAuthorityGuards() {
  auto fixture = mixedFixture(true);
  auto plan = admit(fixture);
  const auto values = input(plan);
  auto candidate = mixedFixture(false).identity;
  candidate.initializers = {{.sequence = 1u, .epoch = 9u, .publicationEdge = 147u,
      .submissionEdge = 520u, .completionEdge = 521u, .completedSequence = 1u,
      .cutEventOrdinal = 5u, .cutKind = static_cast<std::uint32_t>(Cut::AfterRecord), .cutRecord = 1u}};
  auto& end = candidate.coordinators[0].provenance;
  end.prefixInitializer = 1u; end.prefixInitializerEpoch = 9u; end.initializerIndex = 0u;
  RenderTapeSemanticMappingV4 mapping;
  check(validateRenderTapeIdentityV4Rows(candidate, values.events, values.localRecords, true) ==
      RenderTapeIdentityV4Status::Valid, "initializer fail-closed fixture first passes every general provenance guard");
  check(build(plan, candidate, values, mapping) == Status::UnsupportedInitializer && empty(mapping),
      "bounded slice explicitly rejects genuine interval initializer correspondence");
  candidate.initializers[0].completionEdge = candidate.initializers[0].completedSequence = 0u;
  check(build(plan, candidate, values, mapping) == Status::InvalidCandidate && empty(mapping),
      "initializer exclusion cannot bypass incomplete lifecycle guards");
  candidate = mixedFixture(false).identity;
  candidate.header.coordinatorDomain = static_cast<std::uint32_t>(TapeCoordinatorAuthority::SessionRelease);
  candidate.coordinators[0].authority = candidate.header.coordinatorDomain;
  candidate.coordinators[0].admissionEdge = 0u;
  candidate.coordinators[0].provenance.releaseOrdinal = 1u;
  candidate.coordinators[0].provenance.releaseGeneration = 2u;
  check(validateRenderTapeIdentityV4Rows(candidate, values.events, values.localRecords, true) ==
      RenderTapeIdentityV4Status::Valid, "alternate coordinator authority is real but a different semantic contract");
  check(build(plan, candidate, values, mapping) == Status::SemanticMismatch && empty(mapping),
      "correspondence retains original coordinator authority instead of fabricating serial admission");
}

void finiteCapacityAndAllocationAtomicity() {
  auto fixture = mixedFixture(false);
  auto plan = admit(fixture);
  const auto values = input(plan);
  const auto candidate = mixedFixture(true).identity;
  RenderTapeSemanticMappingV4 mapping;
  for (const auto member : {&RenderTapeSemanticMappingV4Limits::maxEvents,
          &RenderTapeSemanticMappingV4Limits::maxRecords, &RenderTapeSemanticMappingV4Limits::maxSources,
          &RenderTapeSemanticMappingV4Limits::maxCommands, &RenderTapeSemanticMappingV4Limits::maxDispositions,
          &RenderTapeSemanticMappingV4Limits::maxCoordinators, &RenderTapeSemanticMappingV4Limits::maxRaws,
          &RenderTapeSemanticMappingV4Limits::maxAttempts, &RenderTapeSemanticMappingV4Limits::maxCommandRelations,
          &RenderTapeSemanticMappingV4Limits::maxSourceRelations}) {
    RenderTapeSemanticMappingV4Limits limits;
    limits.*member = 0u;
    check(build(plan, candidate, values, mapping, limits) == Status::CapacityExceeded && empty(mapping),
        "each explicit zero capacity rejects the required nonempty extent atomically");
    limits = {}; limits.*member = kRenderTapeIdentityV4MaxRows + 1u;
    check(build(plan, candidate, values, mapping, limits) == Status::CapacityExceeded && empty(mapping),
        "limits cannot override the hard finite row bound");
  }
  RenderTapeSemanticMappingV4Limits limits;
  limits.maxCommandRelations = 3u;
  check(build(plan, candidate, values, mapping, limits) == Status::CapacityExceeded && empty(mapping),
      "split command relation capacity is enforced on actual origin pairs");
  limits = {}; limits.maxSourceRelations = 2u;
  check(build(plan, candidate, values, mapping, limits) == Status::CapacityExceeded && empty(mapping),
      "many-to-many source relation capacity rejects without a partial map");
  bool reachedSuccess = false;
  std::size_t rejectedAllocations = 0u;
  for (std::size_t allocation = 0u; allocation < 256u; ++allocation) {
    check(build(plan, candidate, values, mapping) == Status::Valid, "seed full output before allocation fault");
    semantic_mapping_allocation_probe::failAfter = allocation;
    semantic_mapping_allocation_probe::enabled = true;
    const auto status = build(plan, candidate, values, mapping);
    semantic_mapping_allocation_probe::enabled = false;
    if (status == Status::Valid) { reachedSuccess = true; break; }
    ++rejectedAllocations;
    check(status == Status::AllocationFailed && empty(mapping),
        "allocation fault at validation, mapping or normalization stage exposes no partial output");
  }
  check(reachedSuccess && rejectedAllocations > 10u, "fault injection traverses every staged allocation");
  check(buildRenderTapeSemanticMappingV4({}, candidate, values.events, values.localRecords,
      values.obligations, {}, mapping) == Status::InvalidPlan && empty(mapping),
      "unadmitted original cannot qualify any candidate publication");
}

void exhaustiveLegalPartitionsAndEndPerturbations() {
  std::uint32_t layouts = 1u;
  for (std::uint32_t draws = 1u; draws <= 5u; ++draws) {
    std::vector<test::IdentityV4Fixture> fixtures;
    std::vector<CaptureSourceReplayPlanV4> plans;
    std::vector<Input> inputs;
    for (std::uint32_t mask = 0u; mask < layouts; ++mask) {
      fixtures.push_back(partitionFixture(draws, mask));
      plans.push_back(admit(fixtures.back()));
      inputs.push_back(input(plans.back()));
    }
    for (std::uint32_t original = 0u; original < layouts; ++original) {
      for (std::uint32_t local = 0u; local < layouts; ++local) {
        auto candidate = fixtures[local].identity;
        // Every candidate last pre-END source is delayed beyond the original
        // cut. The immutable END endpoint still covers only preceding draws.
        auto& last = candidate.sources[candidate.coordinators[0].provenance.sourceIndex].lifecycle;
        last.cutEventOrdinal = 6u; last.cutKind = static_cast<std::uint32_t>(Cut::BeforeEvent);
        last.cutRecord = 0u; last.publicationEdge = 194u;
        RenderTapeSemanticMappingV4 mapping;
        check(build(plans[original], candidate, inputs[original], mapping) == Status::Valid &&
            mapping.records().size() == draws * 2u + 2u &&
            mapping.coordinators()[0].prefixRecordCount == draws,
            "all bounded legal original/candidate command-source partitions preserve canonical semantics");
        ++partitionPairs;
        // Each candidate prefix is independently perturbed once per domain,
        // avoiding repeating the same counterexample for every original.
        if (original) continue;
        const auto& authenticEnd = candidate.coordinators[0].provenance;
        const auto requiredSource = authenticEnd.sourceIndex;
        for (std::uint32_t source = 0u; source < candidate.sources.size(); ++source) {
          for (std::uint32_t commandEnd = 1u;
               commandEnd <= candidate.sources[source].lifecycle.commandCount; ++commandEnd) {
            auto perturbed = candidate;
            auto& end = perturbed.coordinators[0].provenance;
            const auto& lifetime = candidate.sources[source].lifecycle;
            end.sourceIndex = source; end.commandEnd = commandEnd;
            end.prefixSequence = lifetime.seqId; end.writerCaptureId = lifetime.writerCaptureId;
            end.writerGeneration = lifetime.writerGeneration; end.storageGeneration = lifetime.storageGeneration;
            const auto status = build(plans[original], perturbed, inputs[original], mapping);
            const bool exactSemanticPrefix = source == requiredSource &&
                commandEnd == lifetime.commandCount;
            check(exactSemanticPrefix ? status == Status::Valid && mapping.valid() :
                    (status == Status::InvalidCandidate || status == Status::InvalidPrefix) && empty(mapping),
                "every bounded END source/command cursor rejects omissions and younger additions");
            ++prefixPerturbations;
          }
        }
        auto baseline = candidate;
        auto& end = baseline.coordinators[0].provenance;
        end.prefixKind = static_cast<std::uint32_t>(Prefix::EmptyBaseline);
        end.sourceIndex = noIndex; end.commandEnd = 0u;
        end.prefixSequence = end.writerCaptureId = end.writerGeneration = end.storageGeneration = 0u;
        check(build(plans[original], baseline, inputs[original], mapping) == Status::InvalidPrefix && empty(mapping),
            "baseline cursor cannot replace any nonempty bounded semantic END prefix");
        ++prefixPerturbations;
      }
    }
    layouts *= 3u;
  }
  check(partitionPairs == 7381u && prefixPerturbations == 768u,
      "exhaustive domain covers every declared partition pair and prefix perturbation");
}

} // namespace

int main() {
  try {
    regroupedDrawsAndDelayedPublication();
    queryTranslationsAndIndependentBaselines();
    malformedCoverageAndPrefixes();
    realRollbackAndRetry();
    initializerAndAuthorityGuards();
    finiteCapacityAndAllocationAtomicity();
    exhaustiveLegalPartitionsAndEndPerturbations();
    std::cout << "render tape semantic mapping V4 checks=" << checks <<
        " partition_pairs=" << partitionPairs << " prefix_perturbations=" << prefixPerturbations << '\n';
    return 0;
  } catch (const std::exception& error) {
    semantic_mapping_allocation_probe::enabled = false;
    std::cerr << error.what() << '\n';
    return 1;
  }
}
