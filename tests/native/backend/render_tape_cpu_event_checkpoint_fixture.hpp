#pragma once

#include "render_tape_cpu_destroy_fixture.hpp"

namespace dxmt9::d3d9::test {

enum class CpuEventCheckpointFixtureFault {
  None,
  StaleCycle,
  StaleIdentity,
  MissingEpoch,
  PendingCompletion,
  OpenBegin,
  Occlusion,
};

inline GeneralCpuDestroyFixture makeGeneralCpuEventCheckpointFixture(
    CpuEventCheckpointFixtureFault fault = CpuEventCheckpointFixtureFault::None) {
  auto fixture = makeGeneralCpuDestroyFixture();
  ImportedRenderTapeView source;
  if (!importPrevalidatedRenderTape(fixture.tape, source)) return {};
  const D9CWireObjectIdentity identity{
      .kind = D9C_CHUNK_HANDLE_KIND_QUERY, .generation = 7u, .objectId = 71u};
  RenderTapeQueryDescriptorV3 descriptor{
      .initialIssueCycle = 47u,
      .checkpoint = {
          .checkpointVersion = D9C_RENDER_TAPE_EVENT_QUERY_CHECKPOINT_VERSION_1,
          .status = D9C_RENDER_TAPE_EVENT_QUERY_CHECKPOINT_COMPLETE,
          .queryType = 8u, .identity = identity, .issueCycle = 47u,
          .coreQueryIdentity = 99u, .deviceEpoch = 7u, .issueOrdinal = 63u,
          .rawOrdinal = 11u, .queueIdentity = 909u, .endSequence = 8u,
          .initializerSequence = 3u, .completedSequence = 8u,
          .completedInitializer = 3u}};
  RenderTapeBuilder builder;
  const auto poll = [&](std::uint64_t cycle, std::uint32_t flags) {
    const RenderTapeQueryGetDataControlV2 payload{
        .flags = flags, .hasOutput = 1u, .dataSize = 4u, .issueCycle = cycle};
    builder.appendOrderedControl({
        .identity = identity,
        .kind = static_cast<std::uint32_t>(RenderTapeControlKind::QueryGetData),
        .disposition = static_cast<std::uint32_t>(RenderTapeControlDisposition::Completed),
        .resultCode = 0, .controlBytes = sizeof(payload), .completionOrdinal = 100u},
        std::as_bytes(std::span(&payload, 1u)));
  };
  bool inserted = false;
  for (std::uint32_t index = 0u; index < source.header.eventCount; ++index) {
    const auto event = source.event(index);
    const auto type = static_cast<RenderTapeEventType>(event.header.type);
    builder.appendRawEvent(type, event.payload);
    if (inserted || type != RenderTapeEventType::CommandChunk) continue;
    RenderTapeCommandChunkHeader header{};
    std::memcpy(&header, event.payload.data(), sizeof(header));
    ImportedChunkView chunk;
    if (!importPrevalidatedCommandChunk(event.payload.subspan(sizeof(header)),
            {.version = header.wireVersion, .recordCount = header.recordCount,
             .handleCount = header.handleCount}, chunk)) return {};
    if (std::none_of(chunk.records.begin(), chunk.records.end(), [](const auto& record) {
          return record.type == D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE;
        })) continue;
    inserted = true;
    // Deliberately JIT after real draw work: setup must already own the query,
    // and activation must not add another END or mid-transcript drain.
    builder.appendObjectDefine(identity,
        static_cast<std::uint32_t>(RenderTapeDescriptorKind::Query),
        std::as_bytes(std::span(&descriptor, 1u)), 0u, {});
    poll(47u, 0u);
    const D9CCommandChunkWireHandleEntry handle{
        .kind = identity.kind, .generation = identity.generation, .objectId = identity.objectId};
    const D9CCommandChunkWireQueryIssue issue{.queryHandleIndex = 0u, .flags = 1u};
    const auto issueChunk = general_cpu_destroy_fixture_detail::singleRecordChunk(
        D9C_COMMAND_RECORD_QUERY_ISSUE, std::as_bytes(std::span(&issue, 1u)),
        std::span(&handle, 1u));
    builder.appendCommandChunk({.recordCount = 1u, .handleCount = 1u}, issueChunk);
    poll(48u, 1u);
    builder.appendObjectDestroy(identity);
  }
  const auto candidate = builder.seal();
  ImportedRenderTapeView rebuilt;
  if (!inserted || !importPrevalidatedRenderTape(candidate, rebuilt)) return {};
  switch (fault) {
  case CpuEventCheckpointFixtureFault::None: break;
  case CpuEventCheckpointFixtureFault::StaleCycle: ++descriptor.checkpoint.issueCycle; break;
  case CpuEventCheckpointFixtureFault::StaleIdentity: ++descriptor.checkpoint.identity.generation; break;
  case CpuEventCheckpointFixtureFault::MissingEpoch: descriptor.checkpoint.deviceEpoch = 0u; break;
  case CpuEventCheckpointFixtureFault::PendingCompletion: --descriptor.checkpoint.completedSequence; break;
  case CpuEventCheckpointFixtureFault::OpenBegin: descriptor.checkpoint.active = 1u; break;
  case CpuEventCheckpointFixtureFault::Occlusion:
    descriptor.type = descriptor.checkpoint.queryType = 9u;
    break;
  }
  RenderTapeBuilder finalized;
  std::uint64_t presentOrdinal = 0u;
  for (std::uint32_t index = 0u; index < rebuilt.header.eventCount; ++index) {
    const auto event = rebuilt.event(index);
    const auto type = static_cast<RenderTapeEventType>(event.header.type);
    if (type == RenderTapeEventType::CommandChunk) presentOrdinal = event.header.ordinal;
    if (type == RenderTapeEventType::ObjectDefine) {
      RenderTapeObjectDefineHeader fixed{};
      std::memcpy(&fixed, event.payload.data(), sizeof(fixed));
      if (fixed.identity.kind == D9C_CHUNK_HANDLE_KIND_QUERY) {
        finalized.appendObjectDefine(identity,
            static_cast<std::uint32_t>(RenderTapeDescriptorKind::Query),
            std::as_bytes(std::span(&descriptor, 1u)), 0u, {});
        continue;
      }
    }
    if (type == RenderTapeEventType::PresentComplete) {
      auto payload = std::vector<std::byte>(event.payload.begin(), event.payload.end());
      RenderTapePresentCompleteHeader fixed{};
      std::memcpy(&fixed, payload.data(), sizeof(fixed));
      fixed.presentOrdinal = presentOrdinal;
      fixed.completionOrdinal = 101u;
      std::memcpy(payload.data(), &fixed, sizeof(fixed));
      finalized.appendRawEvent(type, payload);
    } else {
      finalized.appendRawEvent(type, event.payload);
    }
  }
  fixture.tape = finalized.seal();
  return fixture;
}

}  // namespace dxmt9::d3d9::test
