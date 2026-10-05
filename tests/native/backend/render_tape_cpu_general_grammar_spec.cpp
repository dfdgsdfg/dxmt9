#include "render_tape_cpu_general_grammar.hpp"

#include "render_tape_completion_fixture.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
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

std::vector<std::byte> supportedRecordChunk(std::size_t records, std::size_t draws,
                                          bool present = false) {
  const auto stateBytes = test::fullStateChunk(false);
  const auto drawBytes = test::fullStateChunk(true);
  const auto presentBytes = test::presentChunk();
  std::array<ImportedChunkView, 3u> imported;
  for (std::size_t index = 0u; index < imported.size(); ++index) {
    const std::array<std::span<const std::byte>, 3u> bytes{
        stateBytes, drawBytes, presentBytes};
    check(importPrevalidatedCommandChunk(bytes[index], {.recordCount = 1u}, imported[index]),
          "supported record fixture imports");
  }
  const auto recordOffset = sizeof(D9CCommandChunkWireHeader);
  const auto arenaOffset = test::alignUp(recordOffset +
      records * sizeof(D9CCommandChunkWireRecordHeader), alignof(std::uint32_t));
  std::vector<std::byte> bytes(arenaOffset);
  for (std::size_t index = 0u; index < records; ++index) {
    const auto& source = imported[present && index + 1u == records ? 2u :
                                 index < draws ? 1u : 0u];
    auto record = source.records.front();
    const auto payload = source.record(0u).payload;
    record.payloadOffset = static_cast<std::uint32_t>(bytes.size() - arenaOffset);
    std::memcpy(bytes.data() + recordOffset + index * sizeof(record), &record, sizeof(record));
    bytes.insert(bytes.end(), payload.begin(), payload.end());
  }
  const D9CCommandChunkWireHeader header{
      .version = D9C_COMMAND_CHUNK_WIRE_VERSION,
      .headerSize = D9C_COMMAND_CHUNK_WIRE_HEADER_SIZE,
      .recordHeaderSize = D9C_COMMAND_CHUNK_WIRE_RECORD_HEADER_SIZE,
      .handleEntrySize = D9C_COMMAND_CHUNK_WIRE_HANDLE_ENTRY_SIZE,
      .recordTableOffset = static_cast<std::uint32_t>(recordOffset),
      .recordCount = static_cast<std::uint32_t>(records),
      .handleTableOffset = static_cast<std::uint32_t>(arenaOffset),
      .payloadArenaOffset = static_cast<std::uint32_t>(arenaOffset),
      .payloadArenaSize = static_cast<std::uint32_t>(bytes.size() - arenaOffset)};
  std::memcpy(bytes.data(), &header, sizeof(header));
  return bytes;
}

// Input admission uses authenticated metadata and does not allocate captured
// blob bytes. This fixture exercises capture 29's dimensions with supported
// records and synthetic, immutable metadata; it is not execution evidence.
struct CaptureSizedPlan : MiniPlan {
  std::vector<std::vector<std::byte>> chunks;
  D9CBufferDesc bufferDescriptor{.size = 128u * 1024u};

  CaptureSizedPlan() {
    plan.events.clear();
    plan.events.push_back({.type = RenderTapeEventType::BootstrapState,
                           .count = 1u});
    plan.definitions.reserve(738u);
    for (std::size_t index = 1u; index < 738u; ++index) {
      plan.definitions.push_back({
          .fixed = {.identity = {.kind = D9C_CHUNK_HANDLE_KIND_BUFFER,
                                 .generation = 1u, .objectId = index + 1u},
                    .descriptorKind = static_cast<std::uint32_t>(RenderTapeDescriptorKind::Buffer),
                    .descriptorBytes = sizeof(bufferDescriptor)},
          .descriptor = std::as_bytes(std::span(&bufferDescriptor, 1u))});
    }
    for (std::uint32_t index = 0u; index < plan.definitions.size(); ++index)
      plan.events.push_back({.type = RenderTapeEventType::ObjectDefine, .index = index});
    plan.mutations.resize(4902u);
    for (std::uint32_t index = 0u; index < plan.mutations.size(); ++index)
      plan.events.push_back({.type = RenderTapeEventType::ResourceMutation, .index = index});
    plan.catalogue.blobs.resize(4644u);
    for (auto& blob : plan.catalogue.blobs) blob.size = 1u;
    plan.catalogue.blobs.back().size = 432080204ull - 4643u;
    plan.commandChunks.clear();
    chunks.reserve(87u);
    std::size_t remainingRecords = 3513u;
    std::size_t remainingDraws = 2799u;
    for (std::uint32_t index = 0u; index < 87u; ++index) {
      const auto records = remainingRecords / (87u - index);
      const auto draws = remainingDraws < records ? remainingDraws : records;
      chunks.push_back(supportedRecordChunk(records, draws, index == 86u));
      plan.commandChunks.push_back({
          .envelope = {.recordCount = static_cast<std::uint32_t>(records)},
          .bytes = chunks.back()});
      plan.events.push_back({.type = RenderTapeEventType::CommandChunk, .index = index});
      remainingRecords -= records;
      remainingDraws -= draws;
    }
    plan.destroys.push_back({.identity = plan.definitions[1].fixed.identity});
    plan.events.push_back({.type = RenderTapeEventType::ObjectDestroy});
    plan.events.push_back({.type = RenderTapeEventType::PresentComplete});
    for (std::uint32_t index = 0u; index < plan.events.size(); ++index)
      plan.events[index].eventIndex = index;
  }
};

void explicitProfileAdmitsCaptureSizedInput() {
  constexpr GeneralCpuAdmissionCaps defaults{};
  static_assert(defaults.maxEvents == 2048u && defaults.maxObjects == 512u &&
                defaults.maxMutations == 1024u && defaults.maxChunks == 128u &&
                defaults.maxRecords == 4096u && defaults.maxRecordsPerChunk == 64u &&
                defaults.maxBlobBytes == 256ull * 1024ull * 1024ull);
  const CaptureSizedPlan fixture;
  const auto legacy = admitGeneralCpuPlan(fixture.plan);
  check(legacy.reason == GeneralCpuAdmissionReason::EventLimit,
        "default profile still rejects capture-sized input");
  const auto caps = generalCpuExtendedInputCaps();
  const auto result = admitGeneralCpuPlan(fixture.plan, caps);
  check(result.accepted() && result.inputCaps == caps &&
            result.counts.events == 5730u && result.counts.objects == 738u &&
            result.counts.mutations == 4902u && result.counts.blobs == 4644u &&
            result.counts.blobBytes == 432080204ull && result.counts.chunks == 87u &&
            result.counts.records == 3513u && result.counts.draws == 2799u,
        "explicit profile admits all supported capture 29 input dimensions");
  check(result.inputCaps != legacy.inputCaps,
        "owned admission evidence exposes a mismatched caller/executor profile");
}

void captureDimensionsHonorExactBounds() {
  const CaptureSizedPlan fixture;
  const auto admitted = admitGeneralCpuPlan(fixture.plan, generalCpuExtendedInputCaps());
  check(admitted.accepted(), "capture fixture admits before boundary checks");
  auto exact = generalCpuExtendedInputCaps();
  exact.maxEvents = admitted.counts.events;
  exact.maxObjects = admitted.counts.objects;
  exact.maxMutations = admitted.counts.mutations;
  exact.maxChunks = admitted.counts.chunks;
  exact.maxRecords = admitted.counts.records;
  exact.maxBlobBytes = admitted.counts.blobBytes;
  exact.maxRecordsPerChunk = 41u;
  exact.maxDraws = admitted.counts.draws;
  exact.maxBlobs = admitted.counts.blobs;
  exact.maxQueryDefinitions = exact.maxObjects;
  exact.maxTotalChunks = admitted.counts.chunks + admitted.counts.bootstrapChunks;
  exact.maxTotalRecords = admitted.counts.records + admitted.counts.bootstrapRecords;
  check(admitGeneralCpuPlan(fixture.plan, exact).accepted(),
        "every capture dimension admits at its exact cap including bootstrap totals");
  const auto reject = [&](auto member, GeneralCpuAdmissionReason reason) {
    auto caps = exact;
    --(caps.*member);
    check(admitGeneralCpuPlan(fixture.plan, caps).reason == reason,
          "one over the selected cap rejects before any execution");
  };
  reject(&GeneralCpuAdmissionCaps::maxEvents, GeneralCpuAdmissionReason::EventLimit);
  reject(&GeneralCpuAdmissionCaps::maxObjects, GeneralCpuAdmissionReason::ObjectLimit);
  reject(&GeneralCpuAdmissionCaps::maxMutations, GeneralCpuAdmissionReason::MutationLimit);
  reject(&GeneralCpuAdmissionCaps::maxChunks, GeneralCpuAdmissionReason::ChunkLimit);
  reject(&GeneralCpuAdmissionCaps::maxRecords, GeneralCpuAdmissionReason::RecordLimit);
  reject(&GeneralCpuAdmissionCaps::maxBlobBytes, GeneralCpuAdmissionReason::BlobBytesLimit);
  reject(&GeneralCpuAdmissionCaps::maxRecordsPerChunk,
         GeneralCpuAdmissionReason::PerChunkRecordLimit);
  reject(&GeneralCpuAdmissionCaps::maxDraws, GeneralCpuAdmissionReason::DrawLimit);
  reject(&GeneralCpuAdmissionCaps::maxBlobs, GeneralCpuAdmissionReason::BlobCountLimit);
  reject(&GeneralCpuAdmissionCaps::maxTotalChunks, GeneralCpuAdmissionReason::ChunkLimit);
  reject(&GeneralCpuAdmissionCaps::maxTotalRecords, GeneralCpuAdmissionReason::RecordLimit);
}

void legacyBootstrapAndBodyRecordDomainsStayBounded() {
  MiniPlan fixture;
  const auto states = supportedRecordChunk(64u, 0u);
  const auto terminal = supportedRecordChunk(64u, 0u, true);
  fixture.plan.bootstrapChunks.assign(64u, {
      .envelope = {.recordCount = 64u}, .bytes = states});
  fixture.plan.commandChunks.assign(64u, {
      .envelope = {.recordCount = 64u}, .bytes = states});
  fixture.plan.commandChunks.back().bytes = terminal;
  fixture.plan.events = {
      {.type = RenderTapeEventType::BootstrapState, .count = 64u},
      {.type = RenderTapeEventType::ObjectDefine, .index = 0u}};
  for (std::uint32_t index = 0u; index < fixture.plan.commandChunks.size(); ++index)
    fixture.plan.events.push_back({.type = RenderTapeEventType::CommandChunk, .index = index});
  fixture.plan.events.push_back({.type = RenderTapeEventType::PresentComplete});
  for (std::uint32_t index = 0u; index < fixture.plan.events.size(); ++index)
    fixture.plan.events[index].eventIndex = index;
  const auto result = admit(fixture);
  check(result.accepted() && result.counts.bootstrapRecords == 4096u &&
            result.counts.records == 4096u && result.counts.bootstrapChunks == 64u &&
            result.counts.chunks == 64u,
        "default preserves both historical record domains at their exact shared work boundary");
  auto caps = GeneralCpuAdmissionCaps{};
  --caps.maxTotalRecords;
  check(admitGeneralCpuPlan(fixture.plan, caps).reason == GeneralCpuAdmissionReason::RecordLimit,
        "combined bootstrap plus body record work rejects at one over its explicit total");
}

void sourceCapacityIsIndependentAndOverflowChecked() {
  const GeneralCpuSourceCapacity capacity{.maxPublications = 3u, .maxProgressSteps = 7u};
  GeneralCpuSourceDemand demand;
  check(addGeneralCpuSourcePublications(3u, demand) &&
            admitGeneralCpuSourceDemand(demand, capacity) == GeneralCpuSourceAdmission::Accepted,
        "three publications admit independently of thousands of input events");
  check(admitGeneralCpuSourceDemand({.publications = 4u, .progressSteps = 4u}, capacity) ==
            GeneralCpuSourceAdmission::PublicationLimit,
        "one extra publication rejects before effects");
  check(admitGeneralCpuSourceDemand({.publications = 3u, .progressSteps = 8u}, capacity) ==
            GeneralCpuSourceAdmission::ProgressLimit,
        "progress has an independent exact bound");
  check(admitGeneralCpuSourceDemand({.publications = 3u, .progressSteps = 2u}, capacity) ==
            GeneralCpuSourceAdmission::ProgressLimit,
        "inconsistent progress/publication demand rejects");
  check(admitGeneralCpuSourceDemand(demand, {.maxPublications = 0u}) ==
            GeneralCpuSourceAdmission::InvalidCapacity,
        "zero source capacity is not unbounded execution");
  check(admitGeneralCpuSourceDemand(demand,
            {.maxPublications = std::numeric_limits<std::size_t>::max()}) ==
            GeneralCpuSourceAdmission::InvalidCapacity,
        "unrepresentable source capacity rejects");
  const GeneralCpuAdmissionCounts queries{.queryPolls = 1u, .queryCheckpoints = 1u};
  check(addGeneralCpuQueryProgressDemand(queries, 1u, demand) && demand.progressSteps == 7u &&
            admitGeneralCpuSourceDemand(demand, capacity) == GeneralCpuSourceAdmission::Accepted,
        "checkpoint and transcript probes contribute to the exact progress demand");
  GeneralCpuSourceDemand overflow{
      .publications = std::numeric_limits<std::size_t>::max(),
      .progressSteps = std::numeric_limits<std::size_t>::max()};
  check(!addGeneralCpuSourcePublications(1u, overflow),
        "checked source addition rejects integer overflow");
  GeneralCpuSourceDemand queryOverflow;
  const GeneralCpuAdmissionCounts tooMany{
      .queryPolls = std::numeric_limits<std::uint32_t>::max() / 64u + 1u};
  check(!addGeneralCpuQueryProgressDemand(tooMany, 64u, queryOverflow) &&
            queryOverflow.progressSteps == 0u,
        "query multiplication rejects before narrowing to uint32 evidence");
  MiniPlan fixture;
  auto caps = GeneralCpuAdmissionCaps{};
  caps.maxEvents = std::numeric_limits<std::size_t>::max();
  check(admitGeneralCpuPlan(fixture.plan, caps).reason == GeneralCpuAdmissionReason::InvalidBudget,
        "unrepresentable input budget rejects");
  caps = {};
  caps.maxTotalRecords = caps.maxRecords - 1u;
  check(admitGeneralCpuPlan(fixture.plan, caps).reason == GeneralCpuAdmissionReason::InvalidBudget,
        "inconsistent domain and total budget rejects");
  caps = {};
  caps.maxMutations = 0u;
  check(admitGeneralCpuPlan(fixture.plan, caps).reason == GeneralCpuAdmissionReason::InvalidBudget,
        "zero unused dimension does not become an implicit unbounded budget");
}

void acceptedPlanRetainsGeneralObjectsAndState() {
  check(render_tape_runtime::detail::generalCpuCommandRecordSupported(
            D9C_COMMAND_RECORD_GENERATE_MIPMAPS),
        "canonical mip generation has a CPU logical-operation observer");
  check(render_tape_runtime::detail::generalCpuCommandRecordSupported(D9C_COMMAND_RECORD_QUERY_ISSUE),
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
    caps.maxTotalChunks = 1u;
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
    check(!render_tape_runtime::detail::generalCpuCommandRecordSupported(type),
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

  {
    auto plan = fixture.plan;
    auto caps = GeneralCpuAdmissionCaps{};
    caps.maxQueryDefinitions = 1u;
    caps.maxQueryIssues = 1u;
    caps.maxQueryPolls = 1u;
    check(admitGeneralCpuPlan(plan, caps).accepted(),
          "query dimensions accept at their exact caps");
    plan.definitions.push_back(plan.definitions.back());
    ++plan.definitions.back().fixed.identity.objectId;
    plan.commandChunks.push_back(plan.commandChunks.back());
    plan.controls.push_back(plan.controls.back());
    plan.controlBytes.push_back(plan.controlBytes.back());
    plan.events.insert(plan.events.begin() + 5u,
        {{.type = RenderTapeEventType::ObjectDefine, .index = 2u},
         {.type = RenderTapeEventType::CommandChunk, .index = 2u},
         {.type = RenderTapeEventType::OrderedControl, .index = 1u}});
    for (std::uint32_t event = 0u; event < plan.events.size(); ++event)
      plan.events[event].eventIndex = event;
    check(admitGeneralCpuPlan(plan, caps).reason == GeneralCpuAdmissionReason::QueryDefinitionLimit,
          "one extra query definition rejects before effects");
    caps.maxQueryDefinitions = 2u;
    check(admitGeneralCpuPlan(plan, caps).reason == GeneralCpuAdmissionReason::QueryIssueLimit,
          "one extra query issue rejects before effects");
    caps.maxQueryIssues = 2u;
    check(admitGeneralCpuPlan(plan, caps).reason == GeneralCpuAdmissionReason::QueryPollLimit,
          "one extra query poll rejects before effects");
    caps.maxQueryPolls = 2u;
    check(admitGeneralCpuPlan(plan, caps).accepted(),
          "all query dimensions admit together at the raised exact caps");
  }

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
    explicitProfileAdmitsCaptureSizedInput();
    captureDimensionsHonorExactBounds();
    legacyBootstrapAndBodyRecordDomainsStayBounded();
    sourceCapacityIsIndependentAndOverflowChecked();
  } catch (const TestFailure& failure) {
    std::cerr << "render_tape_cpu_general_grammar_spec: " << failure.what()
              << '\n';
    return 1;
  }
  return 0;
}
