#include "device_c_render_tape_source_plan_v4.hpp"
#include "render_tape_identity_v4_fixture.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace {

using namespace dxmt9::d3d9;
using Status = CaptureSourceReplayPlanV4Status;
using Action = CaptureSourceReplayPlanV4Action;
using ActionKind = CaptureSourceReplayPlanV4ActionKind;
using CursorStatus = CaptureSourceReplayCursorV4Status;
using Cut = RenderTapeIdentityV3CutKind;
using Kind = RenderTapeIdentityV3DispositionKind;
using Prefix = RenderTapeIdentityV3PrefixKind;
constexpr auto noIndex = kRenderTapeIdentityV4NoIndex;
constexpr D9CWireObjectIdentity output{D9C_CHUNK_HANDLE_KIND_SURFACE, 1u, 51u};
constexpr D9CWireObjectIdentity query{D9C_CHUNK_HANDLE_KIND_QUERY, 3u, 42u};
std::uint64_t checks = 0u;

void check(bool value, const char* message) {
  ++checks;
  if (!value) throw std::runtime_error(message);
}

struct Fixture {
  std::vector<std::byte> tape, identityBytes, topologyBytes;
  RenderTapeBlobCatalogue catalogue;
  RenderTapeIdentityV4View identity;
  RenderTapeExecutionTopologyV1View topology;
};

void seal(Fixture& fixture) {
  check(validateRenderTape(fixture.tape, fixture.catalogue).valid(),
      "fixture passes full canonical tape validation before V4 admission");
  RenderTapeIdentityV4Status identityStatus{};
  fixture.identityBytes = buildRenderTapeIdentityV4(
      fixture.tape, fixture.catalogue, fixture.identity, &identityStatus);
  if (identityStatus != RenderTapeIdentityV4Status::Valid)
    std::cerr << "fixture identity status=" << static_cast<std::uint32_t>(identityStatus) << '\n';
  check(identityStatus == RenderTapeIdentityV4Status::Valid && !fixture.identityBytes.empty(),
      "fixture is genuine fully admitted V4 provenance, never converted from V3");
  RenderTapeExecutionTopologyV1Status topologyStatus{};
  fixture.topologyBytes = buildRenderTapeExecutionTopologyV1(fixture.tape,
      fixture.catalogue, fixture.identityBytes, fixture.topology, &topologyStatus);
  if (topologyStatus != RenderTapeExecutionTopologyV1Status::Valid)
    std::cerr << "fixture topology status=" << static_cast<std::uint32_t>(topologyStatus) << '\n';
  check(topologyStatus == RenderTapeExecutionTopologyV1Status::Valid && !fixture.topologyBytes.empty(),
      "fixture has a independently bound accepted topology companion");
}

Fixture simpleFixture() {
  auto values = test::makeIdentityV4Fixture();
  Fixture fixture{.tape = std::move(values.tape), .identity = std::move(values.identity),
      .topology = std::move(values.topology)};
  seal(fixture);
  return fixture;
}

// One immutable raw has two actually committed Direct spans and an observed
// pre-effect suffix rollback between them. Both Direct spans share a delayed
// Writing source; their ephemeral build tickets need not equal its publication
// metadata. This trace is evidence, not an allocator-failure replay program.
Fixture spanFixture(bool zeroRecordRollback = false, bool emptyEnd = false) {
  Fixture fixture;
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
  const RenderTapeQueryDescriptorV2 queryDescriptor{.type = 8u, .dataBytes = 4u};
  builder.appendObjectDefine(query, static_cast<std::uint32_t>(RenderTapeDescriptorKind::Query),
      std::as_bytes(std::span(&queryDescriptor, 1u)), 0u, {});
  const D9CWireObjectIdentity buffer{D9C_CHUNK_HANDLE_KIND_BUFFER, 1u, 70u};
  const D9CBufferDesc bufferDescriptor{.size = 16u, .pool = 1u};
  builder.appendObjectDefine(buffer, static_cast<std::uint32_t>(RenderTapeDescriptorKind::Buffer),
      std::as_bytes(std::span(&bufferDescriptor, 1u)), 0u, {}, 16u, 1u);
  const std::array<std::byte, 16u> blob{std::byte{9u}};
  const auto blobDigest = RenderTapeCaptureSession::sha256(blob);
  fixture.catalogue.blobs.push_back({.digest = blobDigest, .size = blob.size(), .verified = 1u});
  builder.appendResourceMutation(buffer, RenderTapeMutationKind::Upload, 0u, 0u, blob.size(), blobDigest);

  const D9CCommandChunkWireDrawHeader draw{
      .primitiveType = 4u, .primitiveCount = 1u,
      .sectionTableOffset = sizeof(D9CCommandChunkWireDrawHeader),
      .sectionPayloadOffset = sizeof(D9CCommandChunkWireDrawHeader)};
  const D9CCommandChunkWireSetConst constants{.registerCount = 1u};
  std::vector<std::byte> constantBytes(sizeof(constants) + 4u * sizeof(float));
  std::memcpy(constantBytes.data(), &constants, sizeof(constants));
  const auto drawBytes = std::as_bytes(std::span(&draw, 1u));
  const std::array payloads{drawBytes, std::span<const std::byte>(constantBytes), drawBytes};
  const std::array types{D9C_COMMAND_RECORD_DRAW_PRIMITIVE,
      D9C_COMMAND_RECORD_SET_VS_CONST_F, D9C_COMMAND_RECORD_DRAW_PRIMITIVE};
  const auto recordsOffset = sizeof(D9CCommandChunkWireHeader);
  const auto payloadOffset = test::alignUp(
      recordsOffset + 3u * sizeof(D9CCommandChunkWireRecordHeader), 8u);
  std::vector<std::byte> payload;
  std::array<D9CCommandChunkWireRecordHeader, 3u> records{};
  for (std::size_t index = 0u; index < payloads.size(); ++index) {
    payload.resize(test::alignUp(payload.size(), 4u));
    records[index] = {.type = types[index], .payloadOffset = static_cast<std::uint32_t>(payload.size()),
        .payloadSize = static_cast<std::uint32_t>(payloads[index].size())};
    payload.insert(payload.end(), payloads[index].begin(), payloads[index].end());
  }
  const D9CCommandChunkWireHeader chunkHeader{
      .version = D9C_COMMAND_CHUNK_WIRE_VERSION, .headerSize = D9C_COMMAND_CHUNK_WIRE_HEADER_SIZE,
      .recordHeaderSize = D9C_COMMAND_CHUNK_WIRE_RECORD_HEADER_SIZE,
      .handleEntrySize = D9C_COMMAND_CHUNK_WIRE_HANDLE_ENTRY_SIZE,
      .recordTableOffset = static_cast<std::uint32_t>(recordsOffset), .recordCount = 3u,
      .handleTableOffset = static_cast<std::uint32_t>(payloadOffset),
      .payloadArenaOffset = static_cast<std::uint32_t>(payloadOffset),
      .payloadArenaSize = static_cast<std::uint32_t>(payload.size())};
  std::vector<std::byte> chunk(payloadOffset + payload.size());
  std::memcpy(chunk.data(), &chunkHeader, sizeof(chunkHeader));
  std::memcpy(chunk.data() + recordsOffset, records.data(), sizeof(records));
  std::memcpy(chunk.data() + payloadOffset, payload.data(), payload.size());
  if (!emptyEnd) builder.appendCommandChunk({.recordCount = 3u}, chunk);
  const D9CCommandChunkWireQueryIssue issue{.flags = 1u};
  const std::array handle{D9CCommandChunkWireHandleEntry{
      .kind = query.kind, .generation = query.generation, .objectId = query.objectId}};
  builder.appendCommandChunk({.recordCount = 1u, .handleCount = 1u}, test::singleRecordChunk(
      D9C_COMMAND_RECORD_QUERY_ISSUE, std::as_bytes(std::span(&issue, 1u)), handle));
  builder.appendCommandChunk({.recordCount = 1u}, test::presentChunk());
  const RenderTapeOracleAttachment oracle{.identity = output,
      .descriptorKind = static_cast<std::uint32_t>(RenderTapeDescriptorKind::Surface)};
  builder.appendPresentComplete(emptyEnd ? 7u : 8u, 1u, RenderTapeDigestValidity::NotCaptured, {},
      std::as_bytes(std::span(&oracle, 1u)));
  fixture.tape = builder.seal();

  auto& identity = fixture.identity;
  identity.header.frameId = 1u; identity.header.presentOrdinal = 8u;
  identity.header.captureToken = 7u; identity.header.captureEpoch = 8u; identity.header.queue = 9u;
  identity.header.initializerEpoch = 9u; identity.header.baselineCompletionEdge = 1u;
  identity.header.baselineKind = static_cast<std::uint32_t>(Prefix::EmptyBaseline);
  identity.sources = {{.lifecycle = {.sourceOrdinal = 1u, .seqId = 1u,
      .writerCaptureId = 1u, .writerGeneration = 2u, .storageGeneration = 3u,
      .publicationEdge = 21u, .submissionEdge = 28u, .completionEdge = 29u,
      .completedSequence = 1u, .cutEventOrdinal = 8u, .commandCount = 2u,
      .cutKind = static_cast<std::uint32_t>(Cut::BeforeEvent)},
      .rawOrdinal = 21u, .publicationBuildGeneration = 12u, .publicationControlIndex = 5u},
      {.lifecycle = {.sourceOrdinal = 2u, .seqId = 2u, .writerCaptureId = 2u,
          .writerGeneration = 2u, .storageGeneration = 3u, .publicationEdge = 24u,
          .submissionEdge = 30u, .completionEdge = 31u, .completedSequence = 2u,
          .cutEventOrdinal = 8u, .commandCount = 1u,
          .cutKind = static_cast<std::uint32_t>(Cut::AfterRecord), .cutRecord = 1u}}};
  identity.initializers = {{.sequence = 1u, .epoch = 9u, .publicationEdge = 15u,
      .submissionEdge = 19u, .completionEdge = 20u, .completedSequence = 1u,
      .cutEventOrdinal = 7u, .cutKind = static_cast<std::uint32_t>(Cut::AfterRecord), .cutRecord = 1u}};
  identity.commands = {{.sourceIndex = 0u,
      .kind = static_cast<std::uint32_t>(RenderTapeIdentityV3CommandKind::DrawRun), .drawCount = 1u},
      {.sourceIndex = 0u, .physicalCommandIndex = 1u,
          .kind = static_cast<std::uint32_t>(RenderTapeIdentityV3CommandKind::DrawRun), .drawCount = 1u},
      {.sourceIndex = 1u, .kind = static_cast<std::uint32_t>(RenderTapeIdentityV3CommandKind::Present)}};
  identity.dispositions = {{.eventOrdinal = 6u, .recordCount = 1u,
      .kind = static_cast<std::uint32_t>(Kind::GpuCommand), .commandIndex = 0u, .subdrawCount = 1u},
      {.eventOrdinal = 6u, .firstRecord = 1u, .recordCount = 1u,
          .kind = static_cast<std::uint32_t>(Kind::StateOnly)},
      {.eventOrdinal = 6u, .firstRecord = 2u, .recordCount = 1u,
          .kind = static_cast<std::uint32_t>(Kind::GpuCommand), .commandIndex = 1u, .subdrawCount = 1u},
      {.eventOrdinal = 7u, .recordCount = 1u, .kind = static_cast<std::uint32_t>(Kind::Coordinator),
          .coordinatorIndex = 0u},
      {.eventOrdinal = 8u, .recordCount = 1u, .kind = static_cast<std::uint32_t>(Kind::GpuCommand),
          .commandIndex = 2u}};
  identity.coordinators = {{.provenance = {.eventOrdinal = 7u, .rawOrdinal = 21u, .buildGeneration = 4u,
      .dispatchEdge = 16u, .releaseOrdinal = 1u, .releaseGeneration = 2u,
      .queryIdentity = query, .issueCycle = 1u, .issueOrdinal = 1u, .deviceEpoch = 77u,
      .writerCaptureId = emptyEnd ? 0u : 1u, .writerGeneration = emptyEnd ? 0u : 2u,
      .storageGeneration = emptyEnd ? 0u : 3u, .prefixSequence = emptyEnd ? 0u : 1u,
      .prefixInitializer = 1u, .prefixInitializerEpoch = 9u,
      .recordType = D9C_COMMAND_RECORD_QUERY_ISSUE, .flags = 1u, .queryType = 8u,
      .prefixKind = static_cast<std::uint32_t>(emptyEnd ? Prefix::EmptyBaseline : Prefix::RealSource),
      .sourceIndex = emptyEnd ? noIndex : 0u, .initializerIndex = 0u,
      .commandEnd = emptyEnd ? 0u : 2u}}};
  identity.settlements = {{6u, 20u, 12u, 3u}, {7u, 21u, 18u, 1u}, {8u, 22u, 26u, 1u}};
  auto prefix = test::compatibilityAttempt(6u, 20u, 1u, 0u, 2u, 2u, 5u);
  auto suffix = test::compatibilityAttempt(6u, 20u, 3u, 2u, 1u, 8u, 11u);
  for (auto* attempt : {&prefix, &suffix}) {
    attempt->effectEdge = attempt == &prefix ? 4u : 9u;
    attempt->stagedCommandCount = 1u;
    attempt->receipt.queueSequence = 1u;
    attempt->receipt.buildGeneration = attempt == &prefix ? 30u : 31u;
    attempt->receipt.sourceGeneration = 2u; attempt->receipt.storageGeneration = 3u;
    attempt->receipt.kind = static_cast<std::uint32_t>(RenderTapeIdentityV4ReceiptKind::DirectChunkSlot);
    attempt->receipt.controlIndex = 5u; attempt->receipt.commandCount = 1u;
  }
  auto rollback = test::compatibilityAttempt(6u, 20u, 2u, 2u,
      zeroRecordRollback ? 0u : 1u, 6u, 7u);
  rollback.outcome = static_cast<std::uint32_t>(RenderTapeIdentityV4AttemptOutcome::RolledBack);
  rollback.receipt = {};
  auto queryAttempt = test::compatibilityAttempt(7u, 21u, 4u, 0u, 1u, 13u, 17u);
  queryAttempt.effectEdge = 14u;
  auto presentAttempt = test::compatibilityAttempt(8u, 22u, 5u, 0u, 1u, 22u, 25u);
  presentAttempt.effectEdge = 23u;
  presentAttempt.stagedCommandCount = presentAttempt.receipt.commandCount = 1u;
  identity.attempts = {prefix, rollback, suffix, queryAttempt, presentAttempt};
  auto& topology = fixture.topology;
  topology.header.captureToken = 7u; topology.header.captureEpoch = 8u; topology.header.queue = 9u;
  topology.plans = {{.planId = 1u, .acceptanceEdge = 27u,
      .route = static_cast<std::uint32_t>(RenderTapeExecutionTopologyV1Route::Optimized),
      .placementCount = 3u, .executedCount = 3u}};
  topology.placements = {{.planIndex = 0u, .commandIndex = 0u,
      .outcome = static_cast<std::uint32_t>(RenderTapeExecutionTopologyV1Outcome::ExecutedDagPass),
      .executionOrdinal = 1u, .dagPassIndex = 17u, .passKind = 1u},
      {.planIndex = 0u, .commandIndex = 1u,
          .outcome = static_cast<std::uint32_t>(RenderTapeExecutionTopologyV1Outcome::ExecutedDagPass),
          .executionOrdinal = 0u, .dagPassIndex = 17u, .passKind = 1u},
      {.planIndex = 0u, .commandIndex = 2u,
          .outcome = static_cast<std::uint32_t>(RenderTapeExecutionTopologyV1Outcome::ExecutedWithoutDag),
          .executionOrdinal = 2u}};
  if (emptyEnd) {
    // Genuine initializer-only END: no preceding render records or unbound
    // writer are relabelled an empty prefix. Only the later Present publishes
    // a render source, independently of this END's initializer dependency.
    identity.header.presentOrdinal = 7u;
    identity.sources.erase(identity.sources.begin());
    identity.sources[0].lifecycle.cutEventOrdinal = 7u;
    identity.commands.erase(identity.commands.begin(), identity.commands.begin() + 2u);
    identity.commands[0].sourceIndex = 0u;
    identity.dispositions.erase(identity.dispositions.begin(), identity.dispositions.begin() + 3u);
    identity.dispositions[0].eventOrdinal = 6u;
    identity.dispositions[1].eventOrdinal = 7u;
    identity.dispositions[1].commandIndex = 0u;
    identity.initializers[0].cutEventOrdinal = 6u;
    identity.coordinators[0].provenance.eventOrdinal = 6u;
    identity.coordinators[0].provenance.rawOrdinal = 20u;
    queryAttempt.eventOrdinal = 6u; queryAttempt.rawOrdinal = 20u;
    queryAttempt.identity.sequence = queryAttempt.receipt.identity.sequence = 20u;
    queryAttempt.receipt.queueSequence = 20u;
    presentAttempt.eventOrdinal = 7u; presentAttempt.rawOrdinal = 21u;
    presentAttempt.identity.sequence = presentAttempt.receipt.identity.sequence = 21u;
    presentAttempt.receipt.queueSequence = 21u;
    identity.attempts = {queryAttempt, presentAttempt};
    identity.settlements = {{6u, 20u, 18u, 1u}, {7u, 21u, 26u, 1u}};
    topology.plans[0].placementCount = topology.plans[0].executedCount = 1u;
    topology.placements.erase(topology.placements.begin(), topology.placements.begin() + 2u);
    topology.placements[0].commandIndex = topology.placements[0].executionOrdinal = 0u;
  }
  seal(fixture);
  return fixture;
}

CaptureSourceReplayPlanV4 admit(const Fixture& fixture) {
  CaptureSourceReplayPlanV4 plan;
  check(buildCaptureSourceReplayPlanV4(fixture.tape, fixture.catalogue,
      fixture.identityBytes, fixture.topologyBytes, plan) == Status::Valid && plan.valid(),
      "the full authenticated V4/topology pair creates one owned plan");
  return plan;
}

void semanticOrderAndEvidence() {
  auto fixture = spanFixture();
  auto plan = admit(fixture);
  check(plan.identity().attempts.size() == 5u && plan.identity().settlements.size() == 3u &&
      plan.events()[5].firstAttemptIndex == 0u && plan.events()[5].attemptCount == 3u &&
      plan.events()[5].rawOrdinal == 20u && plan.events()[5].rawSettlementIndex == 0u,
      "raw event owns all actual span attempts and a separate genuine ledger acknowledgment");
  const std::array expectedAttempts{0u, 0u, 2u, 3u, 4u};
  std::size_t record = 0u, acknowledgments = 0u, sources = 0u, initializers = 0u;
  for (const auto& action : plan.actions()) {
    if (action.kind == ActionKind::DispatchRecord) {
      check(record < expectedAttempts.size() && action.attemptIndex == expectedAttempts[record] &&
          action.eventOrdinal == plan.records()[record].eventOrdinal &&
          action.recordIndex == plan.records()[record].recordIndex,
          "each original record occurs once, joined only to its captured containing committed span");
      ++record;
    }
    acknowledgments += action.kind == ActionKind::AcknowledgeRaw;
    sources += action.kind == ActionKind::ExpectSourcePublication;
    initializers += action.kind == ActionKind::ExpectInitializerPublication;
  }
  check(record == 5u && acknowledgments == 3u && sources == 2u && initializers == 1u,
      "rollback adds no semantic dispatch, publication or raw acknowledgment action");
  check(plan.identity().attempts[0].receipt.kind == static_cast<std::uint32_t>(
      RenderTapeIdentityV4ReceiptKind::DirectChunkSlot) &&
      plan.identity().attempts[0].receipt.buildGeneration != plan.identity().sources[0].publicationBuildGeneration &&
      plan.topology().placements[0].executionOrdinal == 1u && plan.records()[0].recordIndex == 0u,
      "Direct receipt semantics and optimized topology survive without reordering semantic records or inventing passes");
  const auto queryDispatch = std::find_if(plan.actions().begin(), plan.actions().end(), [](const auto& action) {
    return action.kind == ActionKind::DispatchRecord && action.eventOrdinal == 7u;
  });
  check(queryDispatch != plan.actions().end() && queryDispatch->tableIndex == 0u &&
      queryDispatch->attemptIndex == 3u && (queryDispatch + 1u)->kind == ActionKind::ExpectInitializerPublication &&
      (queryDispatch + 1u)->cutKind == static_cast<std::uint32_t>(Cut::AfterRecord) &&
      plan.identity().coordinators[0].provenance.commandEnd == 2u,
      "QUERY_END retains its exact coordinator/cursor and initializer receipt cut, not an unconditional second flush");
  const auto publication = std::find_if(plan.actions().begin(), plan.actions().end(), [](const auto& action) {
    return action.kind == ActionKind::ExpectSourcePublication && action.tableIndex == 0u;
  });
  check(publication != plan.actions().end() && publication->eventOrdinal == 8u &&
      publication->cutKind == static_cast<std::uint32_t>(Cut::BeforeEvent) &&
      publication->evidenceEdge == 21u,
      "cross-event Writing source has one authenticated delayed publication rather than one source per transaction");
  const auto digest = plan.digest();
  fixture.tape.clear(); fixture.catalogue.blobs.clear();
  fixture.identityBytes.clear(); fixture.topologyBytes.clear();
  fixture.identity.attempts.clear(); fixture.topology.placements.clear();
  check(plan.valid() && plan.digest() == digest && plan.identity().attempts.size() == 5u &&
      plan.topology().placements.size() == 3u && plan.referencedBlobs().size() == 1u,
      "plan owns every table/fact/reference and retains no caller storage span");
  auto zero = admit(spanFixture(true));
  check(zero.identity().attempts[1].recordCount == 0u && zero.actions().size() == plan.actions().size() &&
      std::equal(zero.actions().begin(), zero.actions().end(), plan.actions().begin()) && zero.digest() != digest,
      "zero-record failed attempt changes authenticated evidence without changing the semantic replay program");
  auto empty = admit(spanFixture(false, true));
  check(empty.identity().coordinators[0].provenance.prefixKind == static_cast<std::uint32_t>(Prefix::EmptyBaseline) &&
      empty.identity().coordinators[0].provenance.commandEnd == 0u && empty.identity().initializers.size() == 1u,
      "explicit empty END render prefix retains its independent post-arm initializer dependency");
}

std::uint64_t readU64(std::span<const std::byte> bytes, std::size_t offset) {
  check(offset + 8u <= bytes.size(), "digest canonical LE field is bounded");
  std::uint64_t value = 0u;
  for (unsigned index = 0u; index < 8u; ++index)
    value |= std::uint64_t{std::to_integer<unsigned>(bytes[offset + index])} << (index * 8u);
  return value;
}

void exactDigestAndPairBinding() {
  auto fixture = spanFixture();
  auto plan = admit(fixture);
  constexpr std::string_view domain = kCaptureSourceReplayPlanV4DigestDomain;
  const auto bytes = plan.digestCanonicalBytes();
  check(bytes.size() > domain.size() + 124u &&
      std::memcmp(bytes.data(), domain.data(), domain.size()) == 0 &&
      bytes[domain.size()] == std::byte{1u} && bytes[domain.size() + 1u] == std::byte{} &&
      readU64(bytes, domain.size() + 4u) == fixture.tape.size() &&
      readU64(bytes, domain.size() + 44u) == fixture.identityBytes.size() &&
      readU64(bytes, domain.size() + 84u) == fixture.topologyBytes.size() &&
      RenderTapeCaptureSession::sha256(bytes) == plan.digest(),
      "new explicit LE digest domain binds exact Tape, V4 identity and topology component lengths/hashes");
  const auto originalDigest = plan.digest();
  fixture.catalogue.blobs.push_back(fixture.catalogue.blobs[0]);
  fixture.catalogue.blobs.push_back({.digest = {}, .size = 99u});
  std::reverse(fixture.catalogue.blobs.begin(), fixture.catalogue.blobs.end());
  auto normalized = admit(fixture);
  check(normalized.digest() == originalDigest && normalized.referencedBlobs().size() == 1u,
      "referenced verified blobs normalize duplicate/order noise and exclude unused catalogue entries");
  auto changed = spanFixture();
  changed.topology.placements[0].executionOrdinal = 0u;
  changed.topology.placements[1].executionOrdinal = 1u;
  seal(changed);
  auto reordered = admit(changed);
  check(reordered.digest() != originalDigest &&
      std::equal(reordered.actions().begin(), reordered.actions().end(), plan.actions().begin()),
      "entire accepted topology affects the digest independently of unchanged semantic traversal");
  CaptureSourceReplayPlanV4 rejected;
  check(buildCaptureSourceReplayPlanV4(fixture.tape, fixture.catalogue, fixture.identityBytes,
      simpleFixture().topologyBytes, rejected) == Status::InvalidTopology && !rejected.valid(),
      "a topology from another valid capture cannot be swapped into the owned pair");
  auto mismatched = changed.topologyBytes;
  RenderTapeExecutionTopologyV1Header header{};
  std::memcpy(&header, mismatched.data(), sizeof(header));
  header.identityDigest[0] ^= std::byte{1u};
  std::memcpy(mismatched.data(), &header, sizeof(header));
  check(buildCaptureSourceReplayPlanV4(changed.tape, changed.catalogue, changed.identityBytes,
      mismatched, rejected) == Status::InvalidTopology,
      "companion identity digest mismatch rejects before any valid plan is exposed");
  fixture.catalogue.blobs.push_back(plan.referencedBlobs()[0]);
  ++fixture.catalogue.blobs.back().size;
  check(buildCaptureSourceReplayPlanV4(fixture.tape, fixture.catalogue, fixture.identityBytes,
      fixture.topologyBytes, rejected) != Status::Valid && rejected.referencedBlobs().empty(),
      "conflicting referenced catalogue lengths cannot hide behind a verified earlier match");
}

template <typename Row, typename Mutate>
void mutateRow(std::vector<std::byte>& bytes, std::uint32_t offset, Mutate mutate) {
  Row value{};
  check(std::uint64_t{offset} + sizeof(value) <= bytes.size(), "negative row mutation is bounded");
  std::memcpy(&value, bytes.data() + offset, sizeof(value));
  mutate(value);
  std::memcpy(bytes.data() + offset, &value, sizeof(value));
}

void malformedTablesAndCuts() {
  const auto fixture = spanFixture();
  for (std::uint32_t mutation = 0u; mutation < 18u; ++mutation) {
    auto identity = fixture.identityBytes;
    auto topology = fixture.topologyBytes;
    RenderTapeIdentityV4Header h{};
    std::memcpy(&h, identity.data(), sizeof(h));
    if (mutation == 0u) ++h.attempts.offset;
    if (mutation == 1u) h.attempts.count = kRenderTapeIdentityV4MaxRows + 1u;
    if (mutation == 2u) h.version = 3u;
    if (mutation == 3u) h.eventsDigest[0] ^= std::byte{1u};
    std::memcpy(identity.data(), &h, sizeof(h));
    if (mutation == 4u) mutateRow<RenderTapeIdentityV4Source>(identity, h.sources.offset,
        [](auto& row) { row.lifecycle.cutEventOrdinal = 5u; });
    if (mutation == 5u) mutateRow<RenderTapeIdentityV4Source>(identity, h.sources.offset,
        [](auto& row) { row.lifecycle.cutKind = static_cast<std::uint32_t>(Cut::AfterRecord); row.lifecycle.cutRecord = 2u; });
    if (mutation == 6u) mutateRow<RenderTapeIdentityV3Disposition>(identity, h.dispositions.offset,
        [](auto& row) { row.firstRecord = 1u; });
    if (mutation == 7u) mutateRow<RenderTapeIdentityV4Coordinator>(identity, h.coordinators.offset,
        [](auto& row) { --row.provenance.commandEnd; });
    if (mutation == 8u) mutateRow<RenderTapeIdentityV4RawSettlement>(identity, h.settlements.offset,
        [](auto& row) { row.replayEdge = 10u; });
    if (mutation == 9u) mutateRow<RenderTapeIdentityV4ReplayAttempt>(identity,
        h.attempts.offset + sizeof(RenderTapeIdentityV4ReplayAttempt),
        [](auto& row) { row.effectEdge = row.beginEdge + 1u; });
    if (mutation == 10u || mutation == 11u || mutation == 17u) {
      RenderTapeExecutionTopologyV1Header t{};
      std::memcpy(&t, topology.data(), sizeof(t));
      if (mutation == 10u) mutateRow<RenderTapeExecutionTopologyV1Placement>(topology, t.placements.offset,
          [](auto& row) { row.executionOrdinal = 0u; });
      if (mutation == 11u) { ++t.placements.offset; std::memcpy(topology.data(), &t, sizeof(t)); }
      if (mutation == 17u) { ++t.captureEpoch; std::memcpy(topology.data(), &t, sizeof(t)); }
    }
    if (mutation == 12u) mutateRow<RenderTapeIdentityV4PhysicalCommand>(identity, h.commands.offset,
        [](auto& row) { ++row.drawCount; });
    if (mutation == 13u) mutateRow<RenderTapeIdentityV3Initializer>(identity, h.initializers.offset,
        [](auto& row) { row.cutRecord = 0u; });
    if (mutation == 14u) mutateRow<RenderTapeIdentityV4Coordinator>(identity, h.coordinators.offset,
        [](auto& row) { ++row.provenance.buildGeneration; });
    if (mutation == 15u) mutateRow<RenderTapeIdentityV4ReplayAttempt>(identity, h.attempts.offset,
        [](auto& row) { ++row.receipt.storageGeneration; });
    if (mutation == 16u) {
      h.requiredExecutionTopologyVersion = 0u;
      std::memcpy(identity.data(), &h, sizeof(h));
    }
    auto plan = admit(fixture);
    CaptureSourceReplayCursorV4 cursor;
    check(cursor.begin(plan), "negative control cursor first binds a valid plan");
    const auto status = buildCaptureSourceReplayPlanV4(
        fixture.tape, fixture.catalogue, identity, topology, plan);
    check(status != Status::Valid && !plan.valid() && plan.digest() == RenderTapeDigest{} &&
        plan.identity().attempts.empty() && plan.topology().placements.empty() &&
        plan.events().empty() && plan.records().empty() && plan.actions().empty() &&
        plan.digestCanonicalBytes().empty(),
        "mutated layout/table/cut/rollback/coordinator/ACK/topology fails closed with no partial owner");
    Action action;
    check(!cursor.peek(plan, action) && cursor.status() == CursorStatus::Rejected,
        "failed plan rebuild invalidates a formerly bound cursor");
  }
  auto plan = admit(fixture);
  check(buildCaptureSourceReplayPlanV4({}, {}, {}, {}, plan) == Status::InvalidInput &&
      !plan.valid() && plan.actions().empty(), "missing pair or tape invalidates prior authority");
}

void cursorAndMoveAuthority() {
  const auto fixture = spanFixture();
  auto plan = admit(fixture);
  CaptureSourceReplayCursorV4 cursor;
  check(cursor.begin(plan), "fresh value cursor binds the complete digest");
  Action action;
  for (std::size_t index = 0u; index < plan.actions().size(); ++index) {
    check(cursor.peek(plan, action) && action == plan.actions()[index] &&
        cursor.acknowledge(plan, action), "cursor visits each expected semantic/cut/ACK action in order");
  }
  check(cursor.status() == CursorStatus::Complete && cursor.acknowledged() == plan.actions().size() &&
      !cursor.peek(plan, action), "complete traversal means exactly-once plan order only");
  check(!cursor.acknowledge(plan, action) && cursor.status() == CursorStatus::Rejected,
      "a repeated acknowledgment after completion rejects");
  // Exhaust every expected/action pairing over this distinguishing trace
  // against the production cursor, including each publication and raw ACK.
  // This checks traversal only, not a second replay/attempt policy model.
  for (std::size_t expected = 0u; expected < plan.actions().size(); ++expected) {
    for (std::size_t supplied = 0u; supplied < plan.actions().size(); ++supplied) {
      CaptureSourceReplayCursorV4 finite;
      check(finite.begin(plan), "finite cursor trace starts bound");
      for (std::size_t prefix = 0u; prefix < expected; ++prefix)
        check(finite.acknowledge(plan, plan.actions()[prefix]), "finite cursor prefix preserves original order");
      check(finite.acknowledge(plan, plan.actions()[supplied]) == (expected == supplied),
          "production cursor admits exactly the current action across all bounded action pairings");
      if (expected == supplied)
        check(!finite.acknowledge(plan, plan.actions()[supplied]),
            "repeated acknowledgment rejects at every semantic/publication/raw boundary");
      else check(finite.status() == CursorStatus::Rejected && finite.acknowledged() == expected,
          "wrong action permanently rejects without advancing the finite cursor");
    }
  }
  for (std::uint32_t mutation = 0u; mutation < 3u; ++mutation) {
    CaptureSourceReplayCursorV4 bad;
    check(bad.begin(plan) && bad.peek(plan, action), "wrong-action fixture starts bound");
    if (mutation == 0u) ++action.eventOrdinal;
    if (mutation == 1u) action = plan.actions()[1u];
    if (mutation == 2u) ++action.attemptIndex;
    check(!bad.acknowledge(plan, action) && bad.status() == CursorStatus::Rejected,
        "stale, out-of-order or mutated action rejects without advancing");
  }
  CaptureSourceReplayCursorV4 changedCursor;
  check(changedCursor.begin(plan), "stale-plan fixture binds original digest");
  auto changed = admit(spanFixture(true));
  check(!changedCursor.peek(changed, action) && changedCursor.status() == CursorStatus::Rejected,
      "same semantic actions with different rollback evidence cannot reuse a bound cursor");
  CaptureSourceReplayCursorV4 beforeMove;
  check(beforeMove.begin(plan), "move fixture starts bound");
  CaptureSourceReplayPlanV4 moved(std::move(plan));
  check(moved.valid() && !plan.valid() && plan.digest() == RenderTapeDigest{} &&
      !beforeMove.peek(plan, action), "moved-from plan loses authority and cannot validate old cursor access");
  CaptureSourceReplayCursorV4 movedCursor;
  check(movedCursor.begin(moved) && movedCursor.peek(moved, action),
      "owned destination retains complete pair/digest traversal authority");
  CaptureSourceReplayPlanV4 assigned;
  assigned = std::move(moved);
  check(assigned.valid() && !moved.valid() && moved.digest() == RenderTapeDigest{},
      "move assignment also invalidates the source authority");
  CaptureSourceReplayCursorV4 repeatedBegin;
  check(repeatedBegin.begin(assigned) && !repeatedBegin.begin(assigned) &&
      repeatedBegin.status() == CursorStatus::Rejected, "cursor cannot silently restart and duplicate effects");
}

} // namespace

int main() {
  try {
    semanticOrderAndEvidence(); exactDigestAndPairBinding(); malformedTablesAndCuts(); cursorAndMoveAuthority();
    std::cout << "render-tape-source-plan-v4-spec: " << checks << " checks passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "render-tape-source-plan-v4-spec: " << error.what() << '\n';
    return 1;
  }
}
