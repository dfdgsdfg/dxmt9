#pragma once

#include "render_tape_cpu_destroy_fixture.hpp"

namespace dxmt9::d3d9::test {

enum class CpuQueryFixtureFault {
  None,
  StaleCycle,
  MissingBegin,
  UncapturedClosedResult,
  LegacyDescriptor,
};

inline GeneralCpuDestroyFixture makeGeneralCpuQueryFixture(
    CpuQueryFixtureFault fault = CpuQueryFixtureFault::None,
    bool reissue = false, bool flushPending = true) {
  using namespace general_cpu_destroy_fixture_detail;
  auto fixture = makeGeneralCpuDestroyFixture();
  ImportedRenderTapeView source;
  if (!importPrevalidatedRenderTape(fixture.tape, source)) return {};
  const D9CWireObjectIdentity queryIdentity{
      .kind = D9C_CHUNK_HANDLE_KIND_QUERY, .generation = 7u, .objectId = 71u};
  const D9CCommandChunkWireHandleEntry queryHandle{
      .kind = queryIdentity.kind, .generation = queryIdentity.generation,
      .objectId = queryIdentity.objectId};
  RenderTapeBuilder builder;
  const auto issue = [&](std::uint32_t flags) {
    const D9CCommandChunkWireQueryIssue fixed{.queryHandleIndex = 0u, .flags = flags};
    const auto chunk = general_cpu_destroy_fixture_detail::singleRecordChunk(D9C_COMMAND_RECORD_QUERY_ISSUE,
        std::as_bytes(std::span(&fixed, 1u)), std::span(&queryHandle, 1u));
    builder.appendCommandChunk(CommandChunkEnvelope{.recordCount = 1u, .handleCount = 1u}, chunk);
  };
  const auto poll = [&](std::uint64_t cycle, bool ready, std::uint32_t flags) {
    RenderTapeQueryGetDataControlV2 payload{
        .flags = flags, .hasOutput = 1u, .definedBytes = ready ? 4u : 0u,
        .dataSize = 4u, .issueCycle = cycle};
    if (ready) {
      // Deliberately distinct from the substituted CPU result. Only the
      // real-Metal lane may claim a sample-byte match against this oracle.
      const std::uint32_t samples = 37u;
      std::memcpy(payload.result.data(), &samples, sizeof(samples));
    }
    builder.appendOrderedControl({
        .identity = queryIdentity,
        .kind = static_cast<std::uint32_t>(RenderTapeControlKind::QueryGetData),
        .disposition = static_cast<std::uint32_t>(ready
            ? RenderTapeControlDisposition::Completed : RenderTapeControlDisposition::Pending),
        .resultCode = ready ? 0 : 1,
        .controlBytes = sizeof(payload),
        .completionOrdinal = ready ? 100u : 0u},
        std::as_bytes(std::span(&payload, 1u)));
  };
  bool sawDraw = false;
  for (std::uint32_t index = 0u; index < source.header.eventCount; ++index) {
    const auto event = source.event(index);
    const auto type = static_cast<RenderTapeEventType>(event.header.type);
    if (type == RenderTapeEventType::BootstrapState) {
      builder.appendRawEvent(type, event.payload);
      RenderTapeQueryDescriptorV2 descriptor{.type = 9u, .dataBytes = 4u};
      if (fault == CpuQueryFixtureFault::UncapturedClosedResult) {
        descriptor.initialState = static_cast<std::uint32_t>(
            RenderTapeQueryInitialState::ClosedWithoutReplayResult);
        descriptor.initialIssueCycle = 5u;
      }
      if (fault == CpuQueryFixtureFault::LegacyDescriptor) {
        const RenderTapeQueryDescriptor legacy{.type = 9u, .dataBytes = 4u};
        builder.appendObjectDefine(queryIdentity,
            static_cast<std::uint32_t>(RenderTapeDescriptorKind::Query),
            std::as_bytes(std::span(&legacy, 1u)), 0u, {});
      } else {
        builder.appendObjectDefine(queryIdentity,
            static_cast<std::uint32_t>(RenderTapeDescriptorKind::Query),
            std::as_bytes(std::span(&descriptor, 1u)), 0u, {});
      }
      if (fault == CpuQueryFixtureFault::UncapturedClosedResult) poll(5u, true, 0u);
      continue;
    }
    bool draw = false;
    if (type == RenderTapeEventType::CommandChunk) {
      RenderTapeCommandChunkHeader fixed{};
      if (event.payload.size() >= sizeof(fixed)) {
        std::memcpy(&fixed, event.payload.data(), sizeof(fixed));
        ImportedChunkView chunk;
        if (importPrevalidatedCommandChunk(event.payload.subspan(sizeof(fixed)),
                CommandChunkEnvelope{.version = fixed.wireVersion,
                                     .recordCount = fixed.recordCount,
                                     .handleCount = fixed.handleCount}, chunk)) {
          for (const auto& record : chunk.records)
            draw |= record.type == D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE;
        }
      }
    }
    if (draw && !sawDraw) {
      sawDraw = true;
      if (fault != CpuQueryFixtureFault::MissingBegin) issue(2u);
      builder.appendRawEvent(type, event.payload);
      issue(1u);
      const auto cycle = fault == CpuQueryFixtureFault::StaleCycle ? 2u : 1u;
      poll(cycle, false, 0u);
      poll(cycle, false, flushPending ? 1u : 0u);
      poll(cycle, true, 0u);
      if (reissue) {
        issue(2u);
        builder.appendRawEvent(type, event.payload);
        issue(1u);
        poll(2u, true, 1u);
      }
      builder.appendObjectDestroy(queryIdentity);
    } else {
      builder.appendRawEvent(type, event.payload);
    }
  }
  auto candidate = builder.seal();
  ImportedRenderTapeView rebuilt;
  if (!importPrevalidatedRenderTape(candidate, rebuilt)) return {};
  RenderTapeBuilder finalized;
  std::uint64_t presentOrdinal = 0u;
  for (std::uint32_t index = 0u; index < rebuilt.header.eventCount; ++index) {
    const auto event = rebuilt.event(index);
    const auto type = static_cast<RenderTapeEventType>(event.header.type);
    if (type == RenderTapeEventType::CommandChunk) presentOrdinal = event.header.ordinal;
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
