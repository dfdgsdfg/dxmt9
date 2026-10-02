#include "device_c_render_tape_source_plan_v3.hpp"
#include "device_c_render_tape_capture.hpp"
#include "device_c_render_tape_descriptors.hpp"
#include "device_c_render_tape_source_plan.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using namespace dxmt9::d3d9;
using Status = CaptureSourceReplayPlanV3Status;
using IdentityStatus = RenderTapeIdentityV3Status;
using ActionKind = CaptureSourceReplayPlanV3ActionKind;
using Kind = RenderTapeIdentityV3DispositionKind;
using Cut = RenderTapeIdentityV3CutKind;
using Prefix = RenderTapeIdentityV3PrefixKind;
constexpr D9CWireObjectIdentity kOutput{D9C_CHUNK_HANDLE_KIND_SURFACE, 1u, 51u};
constexpr D9CWireObjectIdentity kQuery{D9C_CHUNK_HANDLE_KIND_QUERY, 3u, 42u};
std::uint64_t checks = 0u;

void check(bool value, const char* message) {
  ++checks;
  if (!value) throw std::runtime_error(message);
}

std::size_t alignUp(std::size_t value, std::size_t alignment) {
  return (value + alignment - 1u) & ~(alignment - 1u);
}

template <typename T>
std::vector<std::byte> bytesOf(const T& value) {
  const auto bytes = std::as_bytes(std::span(&value, 1u));
  return {bytes.begin(), bytes.end()};
}

std::vector<std::byte> oneRecord(std::uint32_t type,
    std::span<const std::byte> payload,
    std::span<const D9CCommandChunkWireHandleEntry> handles = {}) {
  const auto recordsOffset = sizeof(D9CCommandChunkWireHeader);
  const auto handlesOffset = alignUp(
      recordsOffset + sizeof(D9CCommandChunkWireRecordHeader), 8u);
  const auto payloadOffset = alignUp(handlesOffset + handles.size_bytes(), 8u);
  const D9CCommandChunkWireHeader header{
      .version = D9C_COMMAND_CHUNK_WIRE_VERSION,
      .headerSize = D9C_COMMAND_CHUNK_WIRE_HEADER_SIZE,
      .recordHeaderSize = D9C_COMMAND_CHUNK_WIRE_RECORD_HEADER_SIZE,
      .handleEntrySize = D9C_COMMAND_CHUNK_WIRE_HANDLE_ENTRY_SIZE,
      .recordTableOffset = static_cast<std::uint32_t>(recordsOffset),
      .recordCount = 1u,
      .handleTableOffset = static_cast<std::uint32_t>(handlesOffset),
      .handleCount = static_cast<std::uint32_t>(handles.size()),
      .payloadArenaOffset = static_cast<std::uint32_t>(payloadOffset),
      .payloadArenaSize = static_cast<std::uint32_t>(payload.size())};
  const D9CCommandChunkWireRecordHeader record{
      .type = type, .payloadSize = static_cast<std::uint32_t>(payload.size()),
      .handleCount = static_cast<std::uint32_t>(handles.size())};
  std::vector<std::byte> bytes(payloadOffset + payload.size());
  std::memcpy(bytes.data(), &header, sizeof(header));
  std::memcpy(bytes.data() + recordsOffset, &record, sizeof(record));
  if (!handles.empty())
    std::memcpy(bytes.data() + handlesOffset, handles.data(), handles.size_bytes());
  if (!payload.empty())
    std::memcpy(bytes.data() + payloadOffset, payload.data(), payload.size());
  return bytes;
}

struct Record {
  std::uint32_t type = 0u;
  std::span<const std::byte> payload;
};

std::vector<std::byte> records(std::span<const Record> input) {
  const auto recordsOffset = sizeof(D9CCommandChunkWireHeader);
  const auto handlesOffset = alignUp(
      recordsOffset + input.size() * sizeof(D9CCommandChunkWireRecordHeader), 8u);
  std::vector<D9CCommandChunkWireRecordHeader> headers;
  std::vector<std::byte> payload;
  for (const auto& record : input) {
    // These fixture records require no handles and at most u32 alignment.
    payload.resize(alignUp(payload.size(), 4u));
    headers.push_back({.type = record.type,
        .payloadOffset = static_cast<std::uint32_t>(payload.size()),
        .payloadSize = static_cast<std::uint32_t>(record.payload.size())});
    payload.insert(payload.end(), record.payload.begin(), record.payload.end());
  }
  const D9CCommandChunkWireHeader header{
      .version = D9C_COMMAND_CHUNK_WIRE_VERSION,
      .headerSize = D9C_COMMAND_CHUNK_WIRE_HEADER_SIZE,
      .recordHeaderSize = D9C_COMMAND_CHUNK_WIRE_RECORD_HEADER_SIZE,
      .handleEntrySize = D9C_COMMAND_CHUNK_WIRE_HANDLE_ENTRY_SIZE,
      .recordTableOffset = static_cast<std::uint32_t>(recordsOffset),
      .recordCount = static_cast<std::uint32_t>(input.size()),
      .handleTableOffset = static_cast<std::uint32_t>(handlesOffset),
      .payloadArenaOffset = static_cast<std::uint32_t>(handlesOffset),
      .payloadArenaSize = static_cast<std::uint32_t>(payload.size())};
  std::vector<std::byte> bytes(handlesOffset + payload.size());
  std::memcpy(bytes.data(), &header, sizeof(header));
  std::memcpy(bytes.data() + recordsOffset, headers.data(), headers.size() * sizeof(headers[0]));
  std::memcpy(bytes.data() + handlesOffset, payload.data(), payload.size());
  return bytes;
}

std::vector<std::byte> bootstrap() {
  std::array<D9CCommandChunkWireTextureBinding, D9C_DRAW_PACKET_MAX_TEXTURES> textures{};
  std::array<D9CCommandChunkWireStreamBinding, D9C_DRAW_PACKET_MAX_STREAMS> streams{};
  for (std::uint32_t index = 0u; index < textures.size(); ++index)
    textures[index] = {.slot = index, .valid = 1u,
        .handleIndex = D9C_COMMAND_CHUNK_NULL_HANDLE_INDEX};
  for (std::uint32_t index = 0u; index < streams.size(); ++index)
    streams[index] = {.slot = index, .valid = 1u,
        .handleIndex = D9C_COMMAND_CHUNK_NULL_HANDLE_INDEX};
  const auto table = sizeof(D9CCommandChunkWireDrawHeader);
  const auto data = alignUp(table + 2u * sizeof(D9CCommandChunkWireSectionDesc), 4u);
  const auto streamsOffset = data + sizeof(textures);
  const D9CCommandChunkWireDrawHeader state{
      .flags = D9C_COMMAND_CHUNK_DRAW_FLAG_FULL_SNAPSHOT, .sectionCount = 2u,
      .sectionTableOffset = static_cast<std::uint32_t>(table),
      .sectionPayloadOffset = static_cast<std::uint32_t>(data)};
  const std::array sections{
      D9CCommandChunkWireSectionDesc{.kind = D9C_COMMAND_CHUNK_SECTION_TEXTURE,
          .elementSize = sizeof(D9CCommandChunkWireTextureBinding),
          .count = D9C_DRAW_PACKET_MAX_TEXTURES,
          .payloadOffset = static_cast<std::uint32_t>(data), .byteSize = sizeof(textures)},
      D9CCommandChunkWireSectionDesc{.kind = D9C_COMMAND_CHUNK_SECTION_STREAM,
          .elementSize = sizeof(D9CCommandChunkWireStreamBinding),
          .count = D9C_DRAW_PACKET_MAX_STREAMS,
          .payloadOffset = static_cast<std::uint32_t>(streamsOffset),
          .byteSize = sizeof(streams)}};
  std::vector<std::byte> bytes(streamsOffset + sizeof(streams));
  std::memcpy(bytes.data(), &state, sizeof(state));
  std::memcpy(bytes.data() + table, sections.data(), sizeof(sections));
  std::memcpy(bytes.data() + data, textures.data(), sizeof(textures));
  std::memcpy(bytes.data() + streamsOffset, streams.data(), sizeof(streams));
  return oneRecord(D9C_COMMAND_RECORD_APPLY_STATE, bytes);
}

enum class FixtureKind { CrossEventWriter, SharedDrawRun, EmptyEnd, InitializerOnly };

struct Fixture {
  std::vector<std::byte> tape;
  RenderTapeBlobCatalogue catalogue;
  RenderTapeIdentityV3View identity;
  std::vector<std::byte> sidecar;
  std::uint64_t stateEvent = 0u;
  std::uint64_t firstGpuEvent = 0u;
  std::uint64_t queryEvent = 0u;
  std::uint64_t secondGpuEvent = 0u;
  std::uint64_t pollEvent = 0u;
  std::uint64_t presentEvent = 0u;
};

void sealIdentity(Fixture& fixture) {
  const auto tapeValidation = validateRenderTape(fixture.tape, fixture.catalogue);
  if (!tapeValidation.valid())
    std::cerr << "fixture tape status=" << renderTapeValidationStatusName(tapeValidation.status)
              << " event=" << tapeValidation.failedEventIndex
              << " chunk=" << static_cast<std::uint32_t>(tapeValidation.chunkStatus) << '\n';
  check(tapeValidation.valid(), "fixture passes complete production tape validation");
  IdentityStatus status{};
  fixture.sidecar = buildRenderTapeIdentityV3(
      fixture.tape, fixture.catalogue, fixture.identity, &status);
  if (status != IdentityStatus::Valid)
    std::cerr << "fixture identity status=" << static_cast<std::uint32_t>(status) << '\n';
  check(status == IdentityStatus::Valid && !fixture.sidecar.empty(),
        "fixture passes the complete production tape and v3 validator");
}

Fixture makeFixture(FixtureKind kind = FixtureKind::CrossEventWriter,
                    bool blobs = false) {
  Fixture fixture;
  RenderTapeBuilder builder;
  std::uint64_t ordinal = 1u;
  builder.appendBootstrapState(bootstrap());
  const RenderTapeSurfaceDescriptorV2 output{
      .schemaVersion = kRenderTapeSurfaceDescriptorVersion2,
      .storage = static_cast<std::uint32_t>(RenderTapeSurfaceStorage::SwapchainBackbuffer),
      .initialContentDisposition = static_cast<std::uint32_t>(
          RenderTapeInitialContentDisposition::ProducedPresentOutput),
      .surface = {.format = 21u, .resourceType = 1u, .usage = 1u,
                  .width = 4u, .height = 4u, .depth = 1u}};
  builder.appendObjectDefine(kOutput, static_cast<std::uint32_t>(
      RenderTapeDescriptorKind::Surface), std::as_bytes(std::span(&output, 1u)), 0u, {});
  ++ordinal;
  const RenderTapeQueryDescriptorV2 descriptor{.type = 8u, .dataBytes = 4u};
  builder.appendObjectDefine(kQuery, static_cast<std::uint32_t>(
      RenderTapeDescriptorKind::Query), std::as_bytes(std::span(&descriptor, 1u)), 0u, {});
  ++ordinal;
  if (blobs) {
    for (std::uint32_t index = 0u; index < 2u; ++index) {
      std::array<std::byte, 16u> seed{};
      seed.fill(static_cast<std::byte>(index + 1u));
      const auto digest = RenderTapeCaptureSession::sha256(seed);
      fixture.catalogue.blobs.push_back({.digest = digest,
          .size = seed.size(), .verified = 1u});
      const D9CWireObjectIdentity buffer{D9C_CHUNK_HANDLE_KIND_BUFFER, 1u, 70u + index};
      const D9CBufferDesc bufferDescriptor{.size = 16u, .pool = 1u};
      builder.appendObjectDefine(buffer, static_cast<std::uint32_t>(
          RenderTapeDescriptorKind::Buffer), std::as_bytes(std::span(&bufferDescriptor, 1u)),
          0u, {}, 16u, 1u);
      ++ordinal;
      builder.appendResourceMutation(buffer, RenderTapeMutationKind::Upload,
          0u, 0u, seed.size(), digest);
      ++ordinal;
    }
  }
  auto& identity = fixture.identity;
  identity.header.frameId = 3u;
  identity.header.captureToken = 5u;
  identity.header.captureEpoch = 6u;
  identity.header.queue = 1u;
  identity.header.baselineKind = static_cast<std::uint32_t>(Prefix::EmptyBaseline);
  identity.header.initializerEpoch = 7u;
  identity.header.baselineInitializer = 2u;
  identity.header.baselineCompletedInitializer = 2u;
  identity.header.baselineCompletionEdge = 1u;
  std::uint64_t raw = 20u;
  const auto command = [&](std::uint32_t type, std::span<const std::byte> payload,
      RenderTapeIdentityV3Disposition disposition,
      std::span<const D9CCommandChunkWireHandleEntry> handles = {}) {
    const auto event = ++ordinal;
    builder.appendCommandChunk({.recordCount = 1u,
        .handleCount = static_cast<std::uint32_t>(handles.size())},
        oneRecord(type, payload, handles));
    disposition.eventOrdinal = event;
    disposition.recordCount = 1u;
    identity.dispositions.push_back(disposition);
    identity.settlements.push_back({event, raw++, 4u, event * 10u + 9u, 1u});
    return event;
  };
  const D9CCommandChunkWireSetConst constants{.registerCount = 1u};
  auto constantBytes = bytesOf(constants);
  constantBytes.resize(sizeof(constants) + 4u * sizeof(float));
  fixture.stateEvent = command(D9C_COMMAND_RECORD_SET_VS_CONST_F, constantBytes,
      {.kind = static_cast<std::uint32_t>(Kind::StateOnly)});
  const bool crossEvent = kind == FixtureKind::CrossEventWriter;
  const bool sharedDraw = kind == FixtureKind::SharedDrawRun;
  const bool querySource = crossEvent || sharedDraw;
  const auto gpuCommand = [&](std::uint32_t recordType, std::uint32_t sourceIndex,
                              std::uint32_t physicalIndex) {
    const auto index = static_cast<std::uint32_t>(identity.commands.size());
    identity.commands.push_back({.sourceIndex = sourceIndex,
        .physicalCommandIndex = physicalIndex,
        .kind = static_cast<std::uint32_t>(
            renderTapeIdentityV3RecordCommandKind(recordType)),
        .membership = static_cast<std::uint32_t>(
            RenderTapeIdentityV3Membership::NoPassCommand)});
    return RenderTapeIdentityV3Disposition{
        .kind = static_cast<std::uint32_t>(Kind::GpuCommand), .commandIndex = index};
  };
  const D9CCommandChunkWireClear clear{
      .flags = 1u, .colorARGB = 0xff204060u, .z = 1.0f,
      .rectOffset = sizeof(D9CCommandChunkWireClear)};
  if (crossEvent)
    fixture.firstGpuEvent = command(D9C_COMMAND_RECORD_CLEAR,
        std::as_bytes(std::span(&clear, 1u)), gpuCommand(D9C_COMMAND_RECORD_CLEAR, 0u, 0u));
  if (sharedDraw) {
    const D9CCommandChunkWireDrawHeader draw{
        .primitiveType = 4u, .primitiveCount = 1u,
        .sectionTableOffset = sizeof(D9CCommandChunkWireDrawHeader),
        .sectionPayloadOffset = sizeof(D9CCommandChunkWireDrawHeader)};
    const auto drawBytes = std::as_bytes(std::span(&draw, 1u));
    const std::array input{
        Record{D9C_COMMAND_RECORD_DRAW_PRIMITIVE, drawBytes},
        Record{D9C_COMMAND_RECORD_SET_VS_CONST_F, constantBytes},
        Record{D9C_COMMAND_RECORD_DRAW_PRIMITIVE, drawBytes}};
    fixture.firstGpuEvent = ++ordinal;
    builder.appendCommandChunk({.recordCount = 3u}, records(input));
    identity.commands.push_back({.logicalPassId = 17u, .sourceIndex = 0u,
        .kind = static_cast<std::uint32_t>(RenderTapeIdentityV3CommandKind::DrawRun),
        .drawCount = 2u,
        .membership = static_cast<std::uint32_t>(RenderTapeIdentityV3Membership::Pass),
        .dagPassIndex = 0u, .passKind = 3u});
    identity.dispositions.push_back({.eventOrdinal = fixture.firstGpuEvent,
        .recordCount = 1u, .kind = static_cast<std::uint32_t>(Kind::GpuCommand),
        .commandIndex = 0u, .subdrawCount = 1u});
    identity.dispositions.push_back({.eventOrdinal = fixture.firstGpuEvent,
        .firstRecord = 1u, .recordCount = 1u,
        .kind = static_cast<std::uint32_t>(Kind::StateOnly)});
    identity.dispositions.push_back({.eventOrdinal = fixture.firstGpuEvent,
        .firstRecord = 2u, .recordCount = 1u,
        .kind = static_cast<std::uint32_t>(Kind::GpuCommand),
        .commandIndex = 0u, .firstSubdraw = 1u, .subdrawCount = 1u});
    identity.settlements.push_back({fixture.firstGpuEvent, raw++, 4u,
        fixture.firstGpuEvent * 10u + 9u, 3u});
  }
  const std::array queryHandle{D9CCommandChunkWireHandleEntry{
      .kind = kQuery.kind, .generation = kQuery.generation, .objectId = kQuery.objectId}};
  const D9CCommandChunkWireQueryIssue issue{.flags = 1u};
  fixture.queryEvent = command(D9C_COMMAND_RECORD_QUERY_ISSUE,
      std::as_bytes(std::span(&issue, 1u)),
      {.kind = static_cast<std::uint32_t>(Kind::Coordinator), .coordinatorIndex = 0u},
      queryHandle);
  const auto querySettlement = identity.settlements.back();
  RenderTapeIdentityV3Coordinator query{
      .eventOrdinal = fixture.queryEvent, .rawOrdinal = querySettlement.rawOrdinal,
      .buildGeneration = 4u, .dispatchEdge = fixture.queryEvent * 10u + 2u,
      .releaseOrdinal = 1u, .releaseGeneration = 2u,
      .queryIdentity = kQuery, .issueCycle = 1u, .issueOrdinal = 1u,
      .deviceEpoch = 77u, .recordType = D9C_COMMAND_RECORD_QUERY_ISSUE,
      .flags = 1u, .queryType = 8u,
      .prefixKind = static_cast<std::uint32_t>(
          querySource ? Prefix::RealSource : Prefix::EmptyBaseline)};
  if (querySource) {
    query.writerCaptureId = 1u;
    query.writerGeneration = 9u;
    query.storageGeneration = 7u;
    query.prefixSequence = 11u;
    query.sourceIndex = 0u;
    query.commandEnd = 1u;
  }
  if (kind == FixtureKind::InitializerOnly) {
    query.prefixInitializer = 5u;
    query.prefixInitializerEpoch = 7u;
    query.initializerIndex = 0u;
    identity.initializers.push_back({.sequence = 5u, .epoch = 7u,
        .publicationEdge = fixture.queryEvent * 10u + 1u,
        .submissionEdge = fixture.queryEvent * 10u + 3u,
        .completionEdge = fixture.queryEvent * 10u + 4u,
        .completedSequence = 5u, .cutEventOrdinal = fixture.queryEvent,
        .cutKind = static_cast<std::uint32_t>(Cut::AfterRecord), .cutRecord = 1u});
  }
  identity.coordinators.push_back(query);
  if (crossEvent)
    fixture.secondGpuEvent = command(D9C_COMMAND_RECORD_CLEAR,
        std::as_bytes(std::span(&clear, 1u)), gpuCommand(D9C_COMMAND_RECORD_CLEAR, 0u, 1u));
  const RenderTapeQueryGetDataControlV2 poll{.issueCycle = 1u};
  fixture.pollEvent = ++ordinal;
  builder.appendOrderedControl({.identity = kQuery,
      .kind = static_cast<std::uint32_t>(RenderTapeControlKind::QueryGetData),
      .disposition = static_cast<std::uint32_t>(RenderTapeControlDisposition::Completed),
      .controlBytes = sizeof(poll),
      .completionOrdinal = 1u}, std::as_bytes(std::span(&poll, 1u)));
  const auto presentSource = querySource ? 1u : 0u;
  const D9CCommandChunkWirePresent present{};
  fixture.presentEvent = command(D9C_COMMAND_RECORD_PRESENT,
      std::as_bytes(std::span(&present, 1u)),
      gpuCommand(D9C_COMMAND_RECORD_PRESENT, presentSource, 0u));
  identity.header.presentOrdinal = fixture.presentEvent;
  if (querySource)
    identity.sources.push_back({.sourceOrdinal = 7u, .seqId = 11u,
        .writerCaptureId = 1u, .writerGeneration = 9u, .storageGeneration = 7u,
        .publicationEdge = fixture.pollEvent * 10u + 1u,
        .submissionEdge = fixture.pollEvent * 10u + 2u,
        .completionEdge = fixture.pollEvent * 10u + 3u,
        .completedSequence = 11u, .cutEventOrdinal = fixture.pollEvent,
        .commandCount = crossEvent ? 2u : 1u,
        .cutKind = static_cast<std::uint32_t>(Cut::BeforeEvent)});
  identity.sources.push_back({.sourceOrdinal = 8u, .seqId = 12u,
      .writerCaptureId = 2u, .writerGeneration = 10u, .storageGeneration = 8u,
      .publicationEdge = fixture.presentEvent * 10u + 1u,
      .submissionEdge = fixture.presentEvent * 10u + 2u,
      .completionEdge = fixture.presentEvent * 10u + 3u,
      .completedSequence = 12u, .cutEventOrdinal = fixture.presentEvent,
      .commandCount = 1u, .cutKind = static_cast<std::uint32_t>(Cut::AfterRecord),
      .cutRecord = 1u});
  const RenderTapeOracleAttachment oracle{.identity = kOutput,
      .descriptorKind = static_cast<std::uint32_t>(RenderTapeDescriptorKind::Surface)};
  builder.appendPresentComplete(fixture.presentEvent, 30u,
      RenderTapeDigestValidity::NotCaptured, {}, std::as_bytes(std::span(&oracle, 1u)));
  fixture.tape = builder.seal();
  sealIdentity(fixture);
  return fixture;
}

CaptureSourceReplayPlanV3 admit(const Fixture& fixture) {
  CaptureSourceReplayPlanV3 plan;
  check(buildCaptureSourceReplayPlanV3(fixture.tape, fixture.catalogue,
            fixture.sidecar, plan) == Status::Valid && plan.valid(),
        "full authenticated v3 input publishes a complete owned plan");
  return plan;
}

void crossEventActionsAndExactlyOnceCursor() {
  const auto fixture = makeFixture();
  auto plan = admit(fixture);
  check(plan.identity().sources.size() == 2u && plan.identity().commands.size() == 3u,
        "two original events share one actual source, not duplicated per-event sources");
  std::size_t dispatched = 0u;
  std::size_t settled = 0u;
  std::size_t published = 0u;
  bool sawFirst = false;
  bool sawSecond = false;
  bool sawEnd = false;
  CaptureSourceReplayCursorV3 cursor;
  check(cursor.begin(plan), "fresh cursor binds one exact admitted plan");
  CaptureSourceReplayPlanV3Action action;
  while (cursor.peek(plan, action)) {
    if (action.kind == ActionKind::DispatchRecord) {
      ++dispatched;
      sawFirst |= action.eventOrdinal == fixture.firstGpuEvent;
      sawSecond |= action.eventOrdinal == fixture.secondGpuEvent;
      sawEnd |= action.eventOrdinal == fixture.queryEvent;
      if (action.eventOrdinal == fixture.queryEvent)
        check(action.tableIndex == 0u && published == 0u,
              "END dispatch settles without an invented publication");
    } else if (action.kind == ActionKind::ExpectSourcePublication) {
      ++published;
      if (action.tableIndex == 0u)
        check(sawFirst && sawSecond && sawEnd && action.eventOrdinal == fixture.pollEvent &&
                  action.cutKind == static_cast<std::uint32_t>(Cut::BeforeEvent),
              "cross-event source is expected once at its original before-event cut");
    } else if (action.kind == ActionKind::SettleCommandEvent) {
      ++settled;
    }
    check(cursor.acknowledge(plan, action), "only the exact next action acknowledges");
  }
  check(cursor.status() == CaptureSourceReplayCursorV3Status::Complete &&
            dispatched == 5u && settled == 5u && published == 2u &&
            cursor.acknowledged() == plan.actions().size(),
        "record and replay settlement counts are independent of actual source count");
  check(!cursor.acknowledge(plan, action) &&
            cursor.status() == CaptureSourceReplayCursorV3Status::Rejected,
        "duplicate terminal acknowledgment cannot execute the stream again");
  check(plan.identity().coordinators[0].commandEnd == 1u &&
            plan.identity().sources[0].commandCount == 2u,
        "younger event command does not expand immutable END prefix");
}

void sharedDrawRunKeepsOriginalRecordTraversal() {
  const auto fixture = makeFixture(FixtureKind::SharedDrawRun);
  const auto plan = admit(fixture);
  check(plan.identity().sources[0].commandCount == 1u &&
            plan.identity().commands.size() == 2u &&
            plan.identity().commands[0].drawCount == 2u,
        "Draw/constant/Draw contributes two subdraws to one actual physical DrawRun");
  std::uint32_t record = 0u;
  std::uint32_t draws = 0u;
  std::size_t sourceActions = 0u;
  CaptureSourceReplayCursorV3 cursor;
  check(cursor.begin(plan), "shared DrawRun cursor binds full admitted input");
  CaptureSourceReplayPlanV3Action action;
  while (cursor.peek(plan, action)) {
    if (action.kind == ActionKind::DispatchRecord &&
        action.eventOrdinal == fixture.firstGpuEvent) {
      check(action.recordIndex == record++ &&
                action.dispositionIndex < plan.identity().dispositions.size(),
            "shared DrawRun dispatches each original record once, including interleaved state");
      const auto& disposition = plan.identity().dispositions[action.dispositionIndex];
      if (disposition.kind == static_cast<std::uint32_t>(Kind::GpuCommand)) {
        check(disposition.commandIndex == 0u && disposition.firstSubdraw == draws++ &&
                  disposition.subdrawCount == 1u,
              "distinct original draw contributors retain their actual shared command/subdraw identity");
      } else {
        check(disposition.kind == static_cast<std::uint32_t>(Kind::StateOnly) &&
                  disposition.commandIndex == kRenderTapeIdentityV3NoIndex,
              "interleaved constant record does not become a physical command");
      }
    }
    sourceActions += action.kind == ActionKind::ExpectSourcePublication;
    check(cursor.acknowledge(plan, action), "shared DrawRun traversal accepts only its next action");
  }
  check(record == 3u && draws == 2u && sourceActions == 2u &&
            cursor.status() == CaptureSourceReplayCursorV3Status::Complete &&
            plan.identity().coordinators[0].commandEnd == 1u,
        "physical command cardinality never substitutes for original record cardinality");
}

void noPublicationAndInitializerCuts() {
  for (auto kind : {FixtureKind::EmptyEnd, FixtureKind::InitializerOnly}) {
    const auto fixture = makeFixture(kind);
    auto plan = admit(fixture);
    std::size_t sourceActions = 0u;
    std::size_t initializerActions = 0u;
    for (const auto& action : plan.actions()) {
      if (action.kind == ActionKind::ExpectSourcePublication) {
        ++sourceActions;
        check(action.eventOrdinal == fixture.presentEvent,
              "empty END/state-only event cannot manufacture a render source");
      }
      if (action.kind == ActionKind::ExpectInitializerPublication) {
        ++initializerActions;
        check(action.eventOrdinal == fixture.queryEvent && action.cutRecord == 1u &&
                  action.cutKind == static_cast<std::uint32_t>(Cut::AfterRecord),
              "initializer-only END retains its independent original publication cut");
      }
    }
    check(sourceActions == 1u && initializerActions ==
              (kind == FixtureKind::InitializerOnly ? 1u : 0u),
          "independent source/initializer tables retain exact publication counts");
    check(plan.identity().coordinators[0].prefixSequence == 0u &&
              plan.identity().coordinators[0].prefixKind == static_cast<std::uint32_t>(
                  Prefix::EmptyBaseline),
          "zero sequence has explicit completed arm authority, not inferred emptiness");
  }
  auto fixture = makeFixture(FixtureKind::EmptyEnd);
  fixture.identity.sources.back().cutKind = static_cast<std::uint32_t>(Cut::AfterEvent);
  fixture.identity.sources.back().cutRecord = 0u;
  sealIdentity(fixture);
  const auto plan = admit(fixture);
  const auto cut = std::find_if(plan.actions().begin(), plan.actions().end(),
      [](const auto& action) { return action.kind == ActionKind::ExpectSourcePublication; });
  check(cut != plan.actions().end() && cut->cutKind == static_cast<std::uint32_t>(Cut::AfterEvent) &&
            cut + 1u != plan.actions().end() &&
            (cut + 1u)->kind == ActionKind::SettleCommandEvent,
        "after-event publication expectation is distinct from replay settlement");
}

void atomicAdmissionAndCursorFailures() {
  const auto fixture = makeFixture();
  auto plan = admit(fixture);
  for (std::uint32_t mutation = 0u; mutation < 5u; ++mutation) {
    plan = admit(fixture);
    CaptureSourceReplayCursorV3 priorCursor;
    check(priorCursor.begin(plan), "prior admission can bind before failed replacement");
    auto bytes = fixture.sidecar;
    RenderTapeIdentityV3Header header{};
    std::memcpy(&header, bytes.data(), sizeof(header));
    if (mutation == 0u) header.version = 2u;
    if (mutation == 1u) header.commands.offset = UINT32_MAX;
    if (mutation == 2u) header.dispositions.count = UINT32_MAX;
    if (mutation == 3u) ++header.eventsBytes;
    if (mutation == 4u) ++header.reserved3;
    std::memcpy(bytes.data(), &header, sizeof(header));
    CaptureSourceReplayPlanV3Action priorAction;
    check(buildCaptureSourceReplayPlanV3(fixture.tape, fixture.catalogue, bytes, plan) != Status::Valid &&
              !plan.valid() && plan.digest() == RenderTapeDigest{} &&
              plan.actions().empty() && plan.events().empty() &&
              plan.identity().sources.empty() && plan.digestCanonicalBytes().empty() &&
              !priorCursor.peek(plan, priorAction),
          "failed admission invalidates prior authority and exposes no partial input plan");
  }
  CaptureSourceReplayPlanV3 invalid;
  check(buildCaptureSourceReplayPlanV3({}, {}, {}, invalid) == Status::InvalidInput &&
            !invalid.valid() && invalid.actions().empty(),
        "empty admission cannot authorize actions");
  plan = admit(fixture);
  auto retained = std::move(plan);
  check(buildCaptureSourceReplayPlanV3({}, {}, {}, plan) == Status::InvalidInput &&
            !plan.valid() && retained.valid(),
        "previous authority survives failed input only when caller moves its owner aside explicitly");
  plan = admit(fixture);
  for (std::uint32_t mutation = 0u; mutation < 3u; ++mutation) {
    CaptureSourceReplayCursorV3 cursor;
    check(cursor.begin(plan), "negative cursor starts on admitted plan");
    CaptureSourceReplayPlanV3Action action;
    check(cursor.peek(plan, action), "negative cursor has one pending action");
    if (mutation == 0u) ++action.eventOrdinal;
    if (mutation == 1u) action = plan.actions()[1u];
    if (mutation == 2u) action.kind = ActionKind::ExpectSourcePublication;
    check(!cursor.acknowledge(plan, action) &&
              cursor.status() == CaptureSourceReplayCursorV3Status::Rejected &&
              !cursor.peek(plan, action) && !cursor.begin(plan),
          "forged/skipped acknowledgment is sticky, not a recoverable partial replay");
  }
  CaptureSourceReplayCursorV3 movedCursor;
  check(movedCursor.begin(plan), "move guard binds original plan");
  auto moved = std::move(plan);
  CaptureSourceReplayPlanV3Action action;
  check(!plan.valid() && moved.valid() && !movedCursor.peek(plan, action),
        "moved-from admission cannot keep cursor authority");
}

template <typename T, typename Mutation>
void mutateRow(std::vector<std::byte>& bytes, std::uint32_t offset,
               Mutation mutation) {
  check(offset <= bytes.size() && sizeof(T) <= bytes.size() - offset,
        "negative table mutation is bounded");
  T row{};
  std::memcpy(&row, bytes.data() + offset, sizeof(row));
  mutation(row);
  std::memcpy(bytes.data() + offset, &row, sizeof(row));
}

void identityAndCompletionFailuresExposeNoPlan() {
  const auto fixture = makeFixture();
  const auto initializerFixture = makeFixture(FixtureKind::InitializerOnly);
  for (std::uint32_t failure = 0u; failure < 7u; ++failure) {
    const auto& input = failure >= 5u ? initializerFixture : fixture;
    auto bytes = input.sidecar;
    RenderTapeIdentityV3Header header{};
    std::memcpy(&header, bytes.data(), sizeof(header));
    if (failure == 0u)
      mutateRow<RenderTapeIdentityV3Source>(bytes, header.sources.offset,
          [](auto& row) { ++row.writerGeneration; });
    if (failure == 1u)
      mutateRow<RenderTapeIdentityV3Coordinator>(bytes, header.coordinators.offset,
          [](auto& row) { ++row.commandEnd; });
    if (failure == 2u)
      mutateRow<RenderTapeIdentityV3Coordinator>(bytes, header.coordinators.offset,
          [](auto& row) { ++row.queryIdentity.generation; });
    if (failure == 3u)
      mutateRow<RenderTapeIdentityV3Coordinator>(bytes, header.coordinators.offset,
          [](auto& row) { ++row.issueCycle; });
    if (failure == 4u)
      mutateRow<RenderTapeIdentityV3Source>(bytes, header.sources.offset,
          [](auto& row) { row.completionEdge = row.completedSequence = 0u; });
    if (failure == 5u)
      mutateRow<RenderTapeIdentityV3Initializer>(bytes, header.initializers.offset,
          [](auto& row) { ++row.epoch; });
    if (failure == 6u)
      mutateRow<RenderTapeIdentityV3Initializer>(bytes, header.initializers.offset,
          [](auto& row) { row.completionEdge = row.completedSequence = 0u; });
    auto output = admit(input);
    check(buildCaptureSourceReplayPlanV3(input.tape, input.catalogue, bytes, output) ==
              Status::InvalidIdentity && !output.valid() && output.actions().empty() &&
              output.referencedBlobs().empty() && output.digestCanonicalBytes().empty(),
          "stale writer/query/prefix or incomplete independent completion cannot authorize replay");
  }
}

std::uint64_t readU64(std::span<const std::byte> bytes, std::size_t offset) {
  check(offset + 8u <= bytes.size(), "canonical digest field is bounded");
  std::uint64_t value = 0u;
  for (unsigned index = 0u; index < 8u; ++index)
    value |= std::uint64_t{std::to_integer<unsigned char>(bytes[offset + index])} << (index * 8u);
  return value;
}

void exactInputAndSixTableDigest() {
  auto fixture = makeFixture(FixtureKind::InitializerOnly, true);
  auto plan = admit(fixture);
  constexpr std::string_view domain = kCaptureSourceReplayPlanV3DigestDomain;
  const auto canonical = plan.digestCanonicalBytes();
  check(canonical.size() > domain.size() &&
            std::memcmp(canonical.data(), domain.data(), domain.size()) == 0 &&
            canonical[domain.size()] == std::byte{1u} &&
            readU64(canonical, domain.size() + 4u) == fixture.tape.size() &&
            readU64(canonical, domain.size() + 44u) == fixture.sidecar.size(),
        "digest is domain-separated/versioned with exact LE input lengths");
  check(RenderTapeCaptureSession::sha256(canonical) == plan.digest(),
        "public canonical bytes reproduce the native digest exactly");
  // 116 semantic-header bytes; six explicit counts. Row widths are field
  // widths, not sizeof(T), and pin every table including actual commands.
  const auto& identity = plan.identity();
  const auto expectedBytes = domain.size() + 4u + 80u + 4u +
      plan.referencedBlobs().size() * 40u + 116u + 6u * 4u +
      identity.sources.size() * 96u + identity.initializers.size() * 72u +
      identity.commands.size() * 48u + identity.dispositions.size() * 40u +
      identity.coordinators.size() * 192u + identity.settlements.size() * 40u;
  check(canonical.size() == expectedBytes && plan.referencedBlobs().size() == 2u &&
            plan.referencedBlobs()[0].digest < plan.referencedBlobs()[1].digest,
        "six table payloads and exact sorted referenced component set have no native padding");
  std::reverse(fixture.catalogue.blobs.begin(), fixture.catalogue.blobs.end());
  RenderTapeDigest extraDigest{};
  extraDigest.fill(std::byte{0xee});
  fixture.catalogue.blobs.push_back({.digest = extraDigest, .size = 99u});
  auto reordered = admit(fixture);
  check(reordered.digest() == plan.digest(),
        "catalogue order and unreferenced entries are excluded, never silently bound as inputs");
  fixture.catalogue.blobs.push_back(fixture.catalogue.blobs.front());
  auto duplicate = admit(fixture);
  check(duplicate.digest() == plan.digest(), "identical verified catalogue duplicates normalize exactly");
  ++fixture.catalogue.blobs.back().size;
  check(buildCaptureSourceReplayPlanV3(fixture.tape, fixture.catalogue,
            fixture.sidecar, reordered) != Status::Valid && !reordered.valid() &&
            reordered.actions().empty() && reordered.digest() == RenderTapeDigest{},
        "conflicting referenced length invalidates output regardless of lookup order");
  auto changed = makeFixture(FixtureKind::InitializerOnly, true);
  ++changed.identity.header.captureEpoch;
  sealIdentity(changed);
  const auto changedPlan = admit(changed);
  check(changedPlan.digest() != plan.digest(), "capture epoch/input identity changes cannot reuse a plan digest");
  CaptureSourceReplayCursorV3 cursor;
  check(cursor.begin(plan), "input-binding cursor starts fresh");
  CaptureSourceReplayPlanV3Action action;
  check(!cursor.peek(changedPlan, action) &&
            cursor.status() == CaptureSourceReplayCursorV3Status::Rejected,
        "fresh authority from another component identity cannot rebind an active cursor");
  const auto v2Rejected = validateRenderTapeIdentity(changed.tape, changed.catalogue, changed.sidecar);
  check(!v2Rejected.valid(), "unchanged v2 decoder remains reachable and rejects v3 bytes");
}

void jointPublicationOrder() {
  auto fixture = makeFixture(FixtureKind::EmptyEnd);
  auto& source = fixture.identity.sources[0];
  fixture.identity.initializers.push_back({.sequence = 5u, .epoch = 7u,
      .publicationEdge = source.publicationEdge - 3u,
      .submissionEdge = source.publicationEdge - 2u,
      .completionEdge = source.publicationEdge - 1u,
      .completedSequence = 5u, .cutEventOrdinal = fixture.presentEvent,
      .cutKind = source.cutKind, .cutRecord = source.cutRecord});
  sealIdentity(fixture);
  const auto plan = admit(fixture);
  auto initializer = std::find_if(plan.actions().begin(), plan.actions().end(),
      [](const auto& action) { return action.kind == ActionKind::ExpectInitializerPublication; });
  check(initializer != plan.actions().end() && initializer + 1u != plan.actions().end() &&
            (initializer + 1u)->kind == ActionKind::ExpectSourcePublication &&
            initializer->publicationEdge < (initializer + 1u)->publicationEdge,
        "same-cut independent domains retain actual publication-edge order");
  fixture.identity.initializers[0].cutEventOrdinal = fixture.queryEvent;
  fixture.identity.initializers[0].publicationEdge = source.publicationEdge + 10u;
  fixture.identity.initializers[0].submissionEdge = source.publicationEdge + 11u;
  fixture.identity.initializers[0].completionEdge = source.publicationEdge + 12u;
  sealIdentity(fixture);
  CaptureSourceReplayPlanV3 rejected;
  check(buildCaptureSourceReplayPlanV3(fixture.tape, fixture.catalogue,
            fixture.sidecar, rejected) == Status::InvalidPublicationOrder && !rejected.valid(),
        "individually valid tables cannot invert actual publication order across original cuts");
}

} // namespace

int main() {
  try {
    crossEventActionsAndExactlyOnceCursor();
    sharedDrawRunKeepsOriginalRecordTraversal();
    noPublicationAndInitializerCuts();
    atomicAdmissionAndCursorFailures();
    identityAndCompletionFailuresExposeNoPlan();
    exactInputAndSixTableDigest();
    jointPublicationOrder();
    // Pure admission/traversal binding only. Excludes queue scheduling,
    // runtime receipt consumption, resource lifetime, query bytes, Metal,
    // original RGBA oracles and wild qualification; no runtime driver exists
    // in this tranche and a complete cursor is not execution evidence.
    std::cout << "render-tape-source-plan-v3-spec: " << checks << " checks passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "render-tape-source-plan-v3-spec: " << error.what() << '\n';
    return 1;
  }
}
