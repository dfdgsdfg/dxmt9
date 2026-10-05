#pragma once

#include "device_c_render_tape_execution_topology_v1.hpp"
#include "../backend/render_tape_completion_fixture.hpp"

namespace dxmt9::d3d9::test {

struct IdentityV4Fixture {
  std::vector<std::byte> tape{};
  RenderTapeIdentityV4View identity{};
  RenderTapeExecutionTopologyV1View topology{};
};

inline RenderTapeIdentityV4ReplayAttempt compatibilityAttempt(
    std::uint64_t event, std::uint64_t raw, std::uint64_t generation,
    std::uint32_t firstRecord, std::uint32_t records,
    std::uint64_t begin, std::uint64_t terminal, std::uint64_t frame = 1u) {
  const RenderTapeIdentityV4ReplaySourceIdentity producer{
      .frame = frame, .source = 1u, .lastSource = 1u, .sequence = raw};
  return {.eventOrdinal = event, .rawOrdinal = raw,
      .attemptGeneration = generation, .beginEdge = begin, .terminalEdge = terminal,
      .identity = producer,
      .receipt = {.identity = producer, .queueSequence = raw,
          .kind = static_cast<std::uint32_t>(RenderTapeIdentityV4ReceiptKind::Compatibility)},
      .firstRecord = firstRecord, .recordCount = records,
      .outcome = static_cast<std::uint32_t>(RenderTapeIdentityV4AttemptOutcome::Committed)};
}

// Declared finite fixture traces, not an inference from physical commands or
// GPU lifecycle: each raw executes in one actual committed compatibility span.
// Its staged count is explicitly zero even when the separate source has draws.
inline void setWholeRawCompatibilityAttempts(RenderTapeIdentityV4View& identity) {
  identity.attempts.clear();
  auto priorReplay = identity.header.baselineCompletionEdge;
  std::uint64_t generation = 4u;
  for (const auto& settlement : identity.settlements) {
    identity.attempts.push_back(compatibilityAttempt(settlement.eventOrdinal,
        settlement.rawOrdinal, generation, 0u, settlement.recordCount,
        priorReplay + 1u, settlement.replayEdge - 1u, identity.header.frameId));
    for (auto& row : identity.coordinators) {
      auto& coordinator = row.provenance;
      if (coordinator.eventOrdinal == settlement.eventOrdinal) {
        coordinator.buildGeneration = generation;
        const auto effect = (row.admissionEdge ? row.admissionEdge : coordinator.dispatchEdge) - 1u;
        if (!identity.attempts.back().effectEdge || effect < identity.attempts.back().effectEdge)
          identity.attempts.back().effectEdge = effect;
      }
    }
    priorReplay = settlement.replayEdge;
    ++generation;
  }
}

inline IdentityV4Fixture makeIdentityV4Fixture() {
  IdentityV4Fixture fixture;
  constexpr D9CWireObjectIdentity output{D9C_CHUNK_HANDLE_KIND_SURFACE, 1u, 51u};
  const RenderTapeSurfaceDescriptorV2 descriptor{
      .storage = static_cast<std::uint32_t>(RenderTapeSurfaceStorage::SwapchainBackbuffer),
      .initialContentDisposition = static_cast<std::uint32_t>(
          RenderTapeInitialContentDisposition::ProducedPresentOutput),
      .surface = {.format = 21u, .resourceType = 1u, .usage = 1u,
                  .width = 4u, .height = 4u, .depth = 1u}};
  RenderTapeBuilder builder;
  builder.appendBootstrapState(fullStateChunk(false));
  builder.appendObjectDefine(output, static_cast<std::uint32_t>(RenderTapeDescriptorKind::Surface),
      std::as_bytes(std::span(&descriptor, 1u)), 0u, {});
  builder.appendCommandChunk({.recordCount = 1u}, fullStateChunk(true));
  builder.appendCommandChunk({.recordCount = 1u}, fullStateChunk(true));
  builder.appendCommandChunk({.recordCount = 1u}, presentChunk());
  const RenderTapeOracleAttachment oracle{.identity = output,
      .descriptorKind = static_cast<std::uint32_t>(RenderTapeDescriptorKind::Surface)};
  builder.appendPresentComplete(5u, 1u, RenderTapeDigestValidity::NotCaptured, {},
      std::as_bytes(std::span(&oracle, 1u)));
  fixture.tape = builder.seal();
  auto& identity = fixture.identity;
  identity.header.frameId = 1u; identity.header.presentOrdinal = 5u;
  identity.header.captureToken = 7u; identity.header.captureEpoch = 8u;
  identity.header.queue = 9u; identity.header.initializerEpoch = 9u;
  identity.header.coordinatorDomain = static_cast<std::uint32_t>(TapeCoordinatorAuthority::OrdinarySerial);
  identity.header.baselineCompletionEdge = 1u;
  identity.header.baselineKind = static_cast<std::uint32_t>(RenderTapeIdentityV3PrefixKind::EmptyBaseline);
  for (std::uint32_t i = 0u; i < 3u; ++i) {
    const bool present = i == 2u;
    identity.sources.push_back({.lifecycle = {.sourceOrdinal = i + 1u, .seqId = i + 1u,
        .writerCaptureId = i + 1u, .writerGeneration = 2u, .storageGeneration = 3u,
        .publicationEdge = 3u + i * 4u, .submissionEdge = 16u + i * 2u,
        .completionEdge = 17u + i * 2u, .completedSequence = i + 1u,
        .cutEventOrdinal = i + 3u, .commandCount = 1u,
        .cutKind = static_cast<std::uint32_t>(RenderTapeIdentityV3CutKind::AfterEvent)}});
    identity.commands.push_back({.sourceIndex = i,
        .kind = static_cast<std::uint32_t>(present ? RenderTapeIdentityV3CommandKind::Present
                                                  : RenderTapeIdentityV3CommandKind::DrawRun),
        .drawCount = present ? 0u : 1u});
    identity.dispositions.push_back({.eventOrdinal = i + 3u, .recordCount = 1u,
        .kind = static_cast<std::uint32_t>(RenderTapeIdentityV3DispositionKind::GpuCommand),
        .commandIndex = i, .subdrawCount = present ? 0u : 1u});
    identity.settlements.push_back({i + 3u, i + 3u, 5u + i * 4u, 1u});
  }
  setWholeRawCompatibilityAttempts(identity);
  auto& topology = fixture.topology;
  topology.header.captureToken = identity.header.captureToken;
  topology.header.captureEpoch = identity.header.captureEpoch;
  topology.header.queue = identity.header.queue;
  topology.plans = {{.planId = 1u, .acceptanceEdge = 14u,
      .route = static_cast<std::uint32_t>(RenderTapeExecutionTopologyV1Route::Optimized),
      .placementCount = 3u, .executedCount = 3u}};
  for (std::uint32_t i = 0u; i < 3u; ++i) {
    const bool present = i == 2u;
    topology.placements.push_back({.planIndex = 0u, .commandIndex = i,
        .outcome = static_cast<std::uint32_t>(present
            ? RenderTapeExecutionTopologyV1Outcome::ExecutedWithoutDag
            : RenderTapeExecutionTopologyV1Outcome::ExecutedDagPass),
        .executionOrdinal = present ? 2u : 1u - i,
        .dagPassIndex = present ? kRenderTapeExecutionTopologyV1NoIndex : 17u,
        .passKind = present ? 0u : 1u});
  }
  return fixture;
}

inline IdentityV4Fixture makeIdentityV4CoordinatorFixture(TapeCoordinatorAuthority authority) {
  IdentityV4Fixture fixture;
  constexpr D9CWireObjectIdentity output{D9C_CHUNK_HANDLE_KIND_SURFACE, 1u, 51u};
  constexpr D9CWireObjectIdentity query{D9C_CHUNK_HANDLE_KIND_QUERY, 1u, 52u};
  const RenderTapeSurfaceDescriptorV2 descriptor{
      .storage = static_cast<std::uint32_t>(RenderTapeSurfaceStorage::SwapchainBackbuffer),
      .initialContentDisposition = static_cast<std::uint32_t>(
          RenderTapeInitialContentDisposition::ProducedPresentOutput),
      .surface = {.format = 21u, .resourceType = 1u, .usage = 1u,
          .width = 4u, .height = 4u, .depth = 1u}};
  const RenderTapeQueryDescriptorV2 queryDescriptor{.type = 9u, .dataBytes = 4u};
  RenderTapeBuilder builder;
  builder.appendBootstrapState(fullStateChunk(false));
  builder.appendObjectDefine(output, static_cast<std::uint32_t>(RenderTapeDescriptorKind::Surface),
      std::as_bytes(std::span(&descriptor, 1u)), 0u, {});
  builder.appendObjectDefine(query, static_cast<std::uint32_t>(RenderTapeDescriptorKind::Query),
      std::as_bytes(std::span(&queryDescriptor, 1u)), 0u, {});
  const D9CCommandChunkWireHandleEntry handle{
      .kind = query.kind, .generation = query.generation, .objectId = query.objectId};
  auto& identity = fixture.identity;
  for (std::uint32_t index = 0u; index < 2u; ++index) {
    const D9CCommandChunkWireQueryIssue issue{.flags = index ? 1u : 2u};
    builder.appendCommandChunk({.recordCount = 1u, .handleCount = 1u},
        singleRecordChunk(D9C_COMMAND_RECORD_QUERY_ISSUE,
            std::as_bytes(std::span(&issue, 1u)), std::span(&handle, 1u)));
    const bool session = authority == TapeCoordinatorAuthority::SessionRelease;
    identity.coordinators.push_back({.provenance = {
        .eventOrdinal = 4u + index, .rawOrdinal = 4u + index,
        .dispatchEdge = 20u + index * 30u,
        .releaseOrdinal = session ? index + 1u : 0u, .releaseGeneration = session ? 1u : 0u,
        .queryIdentity = query, .issueCycle = 1u, .issueOrdinal = index + 1u, .deviceEpoch = 1u,
        .recordType = D9C_COMMAND_RECORD_QUERY_ISSUE, .flags = issue.flags, .queryType = 9u,
        .prefixKind = static_cast<std::uint32_t>(index
            ? RenderTapeIdentityV3PrefixKind::EmptyBaseline : RenderTapeIdentityV3PrefixKind::None)},
        .admissionEdge = session ? 0u : 19u + index * 30u,
        .authority = static_cast<std::uint32_t>(authority)});
    identity.dispositions.push_back({.eventOrdinal = 4u + index, .recordCount = 1u,
        .kind = static_cast<std::uint32_t>(RenderTapeIdentityV3DispositionKind::Coordinator),
        .coordinatorIndex = index});
    identity.settlements.push_back({4u + index, 4u + index, 40u + index * 30u, 1u});
  }
  builder.appendCommandChunk({.recordCount = 1u}, presentChunk());
  const RenderTapeOracleAttachment oracle{.identity = output,
      .descriptorKind = static_cast<std::uint32_t>(RenderTapeDescriptorKind::Surface)};
  builder.appendPresentComplete(6u, 1u, RenderTapeDigestValidity::NotCaptured, {},
      std::as_bytes(std::span(&oracle, 1u)));
  fixture.tape = builder.seal();
  identity.header.frameId = 1u; identity.header.presentOrdinal = 6u;
  identity.header.captureToken = 7u; identity.header.captureEpoch = 8u;
  identity.header.queue = identity.header.initializerEpoch = 9u;
  identity.header.baselineCompletionEdge = 1u;
  identity.header.baselineKind = static_cast<std::uint32_t>(RenderTapeIdentityV3PrefixKind::EmptyBaseline);
  identity.header.coordinatorDomain = static_cast<std::uint32_t>(authority);
  identity.sources = {{.lifecycle = {.sourceOrdinal = 1u, .seqId = 1u, .writerCaptureId = 1u,
      .writerGeneration = 2u, .storageGeneration = 3u, .publicationEdge = 80u,
      .submissionEdge = 90u, .completionEdge = 91u, .completedSequence = 1u,
      .cutEventOrdinal = 6u, .commandCount = 1u,
      .cutKind = static_cast<std::uint32_t>(RenderTapeIdentityV3CutKind::AfterEvent)}}};
  identity.commands = {{.sourceIndex = 0u,
      .kind = static_cast<std::uint32_t>(RenderTapeIdentityV3CommandKind::Present)}};
  identity.dispositions.push_back({.eventOrdinal = 6u, .recordCount = 1u,
      .kind = static_cast<std::uint32_t>(RenderTapeIdentityV3DispositionKind::GpuCommand), .commandIndex = 0u});
  identity.settlements.push_back({6u, 6u, 100u, 1u});
  setWholeRawCompatibilityAttempts(identity);
  fixture.topology.header.captureToken = identity.header.captureToken;
  fixture.topology.header.captureEpoch = identity.header.captureEpoch;
  fixture.topology.header.queue = identity.header.queue;
  fixture.topology.plans = {{.planId = 1u, .acceptanceEdge = 85u,
      .route = static_cast<std::uint32_t>(RenderTapeExecutionTopologyV1Route::Optimized),
      .placementCount = 1u, .executedCount = 1u}};
  fixture.topology.placements = {{.planIndex = 0u, .commandIndex = 0u,
      .outcome = static_cast<std::uint32_t>(RenderTapeExecutionTopologyV1Outcome::ExecutedWithoutDag),
      .executionOrdinal = 0u}};
  return fixture;
}

} // namespace dxmt9::d3d9::test
