#include "device_c_cpu_ready_plan.hpp"

#include <algorithm>
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

using dxmt9::d3d9::ImportedChunkV2View;
using dxmt9::d3d9::RawCommandChunk;
using dxmt9::d3d9::V2ChunkEnvelope;
using dxmt9::d3d9::V2ReplayLane;
using dxmt9::d3d9::V2ReplayReason;

struct TestFailure : std::runtime_error {
  using std::runtime_error::runtime_error;
};

void check(bool condition, std::string_view message) {
  if (!condition) {
    throw TestFailure(std::string(message));
  }
}

std::size_t alignUp(std::size_t value, std::size_t alignment) {
  return (value + alignment - 1u) & ~(alignment - 1u);
}

template <typename T>
std::vector<std::byte> bytesOf(const T& value) {
  std::vector<std::byte> bytes(sizeof(T));
  std::memcpy(bytes.data(), &value, sizeof(value));
  return bytes;
}

struct RecordSpec {
  std::uint32_t type = 0;
  std::vector<std::byte> payload;
  std::vector<D9CCommandChunkWireHandleEntryV2> handles;
};

struct ValidatedFixture {
  std::vector<std::byte> bytes;
  V2ChunkEnvelope envelope{};

  ImportedChunkV2View view() const {
    ImportedChunkV2View imported;
    const auto result = dxmt9::d3d9::validateCommandChunkV2(
        bytes, envelope, &imported);
    if (!result.valid()) {
      throw TestFailure(
          "planner fixture validation status " +
          std::to_string(static_cast<unsigned>(result.status)) +
          " at record " + std::to_string(result.failedRecordIndex));
    }
    return imported;
  }
};

ValidatedFixture makeValidatedFixture(std::span<const RecordSpec> specs) {
  std::vector<D9CCommandChunkWireRecordHeaderV2> records;
  std::vector<D9CCommandChunkWireHandleEntryV2> handles;
  std::vector<std::byte> payload;
  for (const auto& spec : specs) {
    const auto* rule = dxmt9::d3d9::v2RecordRule(spec.type);
    check(rule != nullptr, "fixture record must exist in the V2 schema");
    payload.resize(alignUp(payload.size(), rule->payloadAlignment));
    records.push_back({
        .type = spec.type,
        .flags = D9C_COMMAND_CHUNK_V2_RECORD_FLAG_NONE,
        .payloadOffset = static_cast<std::uint32_t>(payload.size()),
        .payloadSize = static_cast<std::uint32_t>(spec.payload.size()),
        .firstHandle = static_cast<std::uint32_t>(handles.size()),
        .handleCount = static_cast<std::uint32_t>(spec.handles.size()),
    });
    payload.insert(payload.end(), spec.payload.begin(), spec.payload.end());
    handles.insert(handles.end(), spec.handles.begin(), spec.handles.end());
  }

  D9CCommandChunkWireHeaderV2 header{
      .version = D9C_COMMAND_CHUNK_WIRE_VERSION_V2,
      .headerSize = D9C_COMMAND_CHUNK_WIRE_HEADER_V2_SIZE,
      .recordHeaderSize = D9C_COMMAND_CHUNK_WIRE_RECORD_HEADER_V2_SIZE,
      .handleEntrySize = D9C_COMMAND_CHUNK_WIRE_HANDLE_ENTRY_V2_SIZE,
      .recordTableOffset = D9C_COMMAND_CHUNK_WIRE_HEADER_V2_SIZE,
      .recordCount = static_cast<std::uint32_t>(records.size()),
      .handleCount = static_cast<std::uint32_t>(handles.size()),
      .payloadArenaSize = static_cast<std::uint32_t>(payload.size()),
  };
  header.handleTableOffset = static_cast<std::uint32_t>(alignUp(
      header.recordTableOffset + records.size() * sizeof(records[0]),
      alignof(D9CCommandChunkWireHandleEntryV2)));
  header.payloadArenaOffset = static_cast<std::uint32_t>(alignUp(
      header.handleTableOffset + handles.size() * sizeof(handles[0]),
      alignof(std::uint32_t)));

  ValidatedFixture fixture;
  fixture.bytes.resize(header.payloadArenaOffset + payload.size());
  std::memcpy(fixture.bytes.data(), &header, sizeof(header));
  if (!records.empty()) {
    std::memcpy(fixture.bytes.data() + header.recordTableOffset,
                records.data(), records.size() * sizeof(records[0]));
  }
  if (!handles.empty()) {
    std::memcpy(fixture.bytes.data() + header.handleTableOffset,
                handles.data(), handles.size() * sizeof(handles[0]));
  }
  if (!payload.empty()) {
    std::memcpy(fixture.bytes.data() + header.payloadArenaOffset,
                payload.data(), payload.size());
  }
  fixture.envelope = {
      .version = D9C_COMMAND_CHUNK_VERSION_V2,
      .recordCount = header.recordCount,
      .handleCount = header.handleCount,
  };
  return fixture;
}

D9CCommandChunkWireHandleEntryV2 handle(std::uint32_t kind,
                                        std::uint64_t objectId) {
  return {
      .kind = kind,
      .generation = 1,
      .objectId = objectId,
  };
}

RecordSpec drawRecord(std::uint32_t type =
                          D9C_COMMAND_RECORD_DRAW_PRIMITIVE,
                      std::uint32_t primitiveType = 4) {
  D9CCommandChunkWireDrawHeaderV2 draw{};
  if (type != D9C_COMMAND_RECORD_APPLY_STATE) {
    draw.primitiveType = primitiveType;
    draw.startVertex = 3;
    draw.primitiveCount = 1;
  }
  draw.sectionTableOffset = sizeof(D9CCommandChunkWireDrawHeaderV2);
  draw.sectionPayloadOffset = sizeof(D9CCommandChunkWireDrawHeaderV2);
  return {.type = type, .payload = bytesOf(draw)};
}

RecordSpec directUpDrawRecord() {
  constexpr std::size_t kVertexBytes = 12;
  D9CCommandChunkWireDrawHeaderV2 draw{
      .primitiveType = 4,
      .primitiveCount = 1,
      .stride = 4,
      .sectionCount = 1,
      .sectionTableOffset = sizeof(D9CCommandChunkWireDrawHeaderV2),
  };
  draw.sectionPayloadOffset = static_cast<std::uint32_t>(alignUp(
      sizeof(draw) + sizeof(D9CCommandChunkWireSectionDescV2),
      alignof(std::uint32_t)));
  const auto* rule = dxmt9::d3d9::v2SectionRule(
      D9C_COMMAND_CHUNK_V2_SECTION_UP_VERTEX_DATA);
  check(rule != nullptr, "UP vertex section rule must exist");
  const D9CCommandChunkWireSectionDescV2 section{
      .kind = D9C_COMMAND_CHUNK_V2_SECTION_UP_VERTEX_DATA,
      .elementSize = rule->elementSize,
      .count = kVertexBytes,
      .payloadOffset = draw.sectionPayloadOffset,
      .byteSize = kVertexBytes,
  };
  std::vector<std::byte> payload(draw.sectionPayloadOffset + kVertexBytes);
  std::memcpy(payload.data(), &draw, sizeof(draw));
  std::memcpy(payload.data() + draw.sectionTableOffset,
              &section, sizeof(section));
  return {
      .type = D9C_COMMAND_RECORD_DRAW_PRIMITIVE_UP,
      .payload = std::move(payload),
  };
}

RecordSpec triangleFanRecord(std::uint32_t type,
                             std::uint32_t indexFormat = 101u,
                             std::uint32_t primitiveCount = 3u,
                             std::uint32_t stride = 4u) {
  D9CCommandChunkWireDrawHeaderV2 draw{
      .primitiveType = 6,
      .primitiveCount = primitiveCount,
      .sectionTableOffset = sizeof(D9CCommandChunkWireDrawHeaderV2),
  };
  std::array<D9CCommandChunkWireSectionDescV2, 2> sections{};
  std::size_t sectionCount = 0;
  std::array<std::size_t, 2> sectionBytes{};
  if (type == D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE) {
    draw.numVertices = primitiveCount + 2u;
  } else if (type == D9C_COMMAND_RECORD_DRAW_PRIMITIVE_UP) {
    draw.stride = stride;
    sectionBytes[sectionCount] =
        static_cast<std::size_t>(primitiveCount + 2u) * stride;
    sections[sectionCount++].kind =
        D9C_COMMAND_CHUNK_V2_SECTION_UP_VERTEX_DATA;
  } else if (type == D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE_UP) {
    draw.numVertices = primitiveCount + 2u;
    draw.stride = stride;
    draw.indexFormat = indexFormat;
    sectionBytes[sectionCount] =
        static_cast<std::size_t>(primitiveCount + 2u) *
        (indexFormat == 102u ? 4u : 2u);
    sections[sectionCount++].kind =
        D9C_COMMAND_CHUNK_V2_SECTION_UP_INDEX_DATA;
    sectionBytes[sectionCount] =
        static_cast<std::size_t>(draw.numVertices) * stride;
    sections[sectionCount++].kind =
        D9C_COMMAND_CHUNK_V2_SECTION_UP_VERTEX_DATA;
  }
  draw.sectionCount = static_cast<std::uint32_t>(sectionCount);
  draw.sectionPayloadOffset = static_cast<std::uint32_t>(alignUp(
      sizeof(draw) + sectionCount * sizeof(sections[0]),
      alignof(std::uint32_t)));
  std::size_t payloadEnd = draw.sectionPayloadOffset;
  for (std::size_t i = 0; i < sectionCount; ++i) {
    const auto* rule = dxmt9::d3d9::v2SectionRule(sections[i].kind);
    check(rule != nullptr, "fan UP section rule must exist");
    payloadEnd = alignUp(payloadEnd, rule->payloadAlignment);
    sections[i].elementSize = rule->elementSize;
    sections[i].count = static_cast<std::uint32_t>(sectionBytes[i]);
    sections[i].payloadOffset = static_cast<std::uint32_t>(payloadEnd);
    sections[i].byteSize = static_cast<std::uint32_t>(sectionBytes[i]);
    payloadEnd += sectionBytes[i];
  }
  std::vector<std::byte> payload(payloadEnd);
  std::memcpy(payload.data(), &draw, sizeof(draw));
  if (sectionCount != 0) {
    std::memcpy(payload.data() + draw.sectionTableOffset, sections.data(),
                sectionCount * sizeof(sections[0]));
  }
  return {.type = type, .payload = std::move(payload)};
}

RecordSpec clearRecord(std::size_t rectCount) {
  D9CCommandChunkWireClearV2 clear{
      .rectCount = static_cast<std::uint32_t>(rectCount),
      .rectOffset = sizeof(D9CCommandChunkWireClearV2),
  };
  auto payload = bytesOf(clear);
  payload.resize(payload.size() + rectCount * sizeof(D9CRect));
  return {.type = D9C_COMMAND_RECORD_CLEAR, .payload = std::move(payload)};
}

std::size_t clearRectCountForSegmentPages(std::size_t targetPages,
                                          bool lastMatch = false) {
  std::size_t match = 0;
  bool found = false;
  for (std::size_t rectCount = 0;
       rectCount <= targetPages * 4096 / sizeof(D9CRect) + 1024;
       ++rectCount) {
    dxmt9::core::SourcePayloadCapacity capacity{};
    capacity.commandHeaders = 1;
    capacity.clearRecords = 1;
    capacity.clearRects = rectCount;
    capacity.drawUniformPayloadLookupHeads = 8;
    capacity.drawUniformPayloadLookupTails = 8;
    capacity.drawUniformVertexConstantsLookupHeads = 8;
    capacity.drawUniformVertexConstantsLookupTails = 8;
    capacity.drawUniformPixelConstantsLookupHeads = 8;
    capacity.drawUniformPixelConstantsLookupTails = 8;
    const auto layout = dxmt9::core::makeSourcePayloadLayout(
        capacity, 4096, std::numeric_limits<std::uint32_t>::max());
    check(layout.has_value(), "clear boundary layout must build");
    if (layout->pageCount == targetPages) {
      match = rectCount;
      found = true;
      if (!lastMatch) {
        return match;
      }
    } else if (found && layout->pageCount > targetPages) {
      return match;
    }
  }
  check(found, "requested clear page boundary must be reachable");
  return match;
}

RecordSpec partialFloatConstantRecord(std::uint32_t type,
                                      std::uint32_t startRegister) {
  const D9CCommandChunkWireSetConstV2 fixed{
      .startRegister = startRegister,
      .registerCount = 1,
  };
  auto payload = bytesOf(fixed);
  payload.resize(payload.size() + 4 * sizeof(float));
  return {.type = type, .payload = std::move(payload)};
}

std::vector<RecordSpec> eligibleRecords() {
  return {
      drawRecord(),
      drawRecord(D9C_COMMAND_RECORD_APPLY_STATE),
      clearRecord(2),
      RecordSpec{
          .type = D9C_COMMAND_RECORD_UPDATE_SURFACE,
          .payload = bytesOf(D9CCommandChunkWireUpdateSurfaceV2{
              .srcHandleIndex = 0, .dstHandleIndex = 1}),
          .handles = {handle(D9C_CHUNK_HANDLE_KIND_SURFACE, 1),
                      handle(D9C_CHUNK_HANDLE_KIND_SURFACE, 2)},
      },
      RecordSpec{
          .type = D9C_COMMAND_RECORD_STRETCH_RECT,
          .payload = bytesOf(D9CCommandChunkWireStretchRectV2{
              .srcHandleIndex = 2, .dstHandleIndex = 3}),
          .handles = {handle(D9C_CHUNK_HANDLE_KIND_SURFACE, 3),
                      handle(D9C_CHUNK_HANDLE_KIND_SURFACE, 4)},
      },
      RecordSpec{
          .type = D9C_COMMAND_RECORD_COLOR_FILL,
          .payload = bytesOf(D9CCommandChunkWireColorFillV2{
              .surfaceHandleIndex = 4}),
          .handles = {handle(D9C_CHUNK_HANDLE_KIND_SURFACE, 5)},
      },
      RecordSpec{
          .type = D9C_COMMAND_RECORD_RESZ_DEPTH_RESOLVE,
          .payload = bytesOf(D9CCommandChunkWireReszDepthResolveV2{
              .msaaDepthHandleIndex = 5, .intzDestHandleIndex = 6}),
          .handles = {handle(D9C_CHUNK_HANDLE_KIND_SURFACE, 6),
                      handle(D9C_CHUNK_HANDLE_KIND_TEXTURE, 7)},
      },
  };
}

void eligibleWholeRawPlanHasTypedCapacity() {
  const auto fixture = makeValidatedFixture(eligibleRecords());
  const auto imported = fixture.view();
  RawCommandChunk raw;
  raw.replaySeq = 77;
  const auto plan = dxmt9::d3d9::planCpuReadyChunkV2(
      imported, raw.replaySeq);
  check(plan.rawOrdinal == raw.replaySeq &&
            plan.lane == V2ReplayLane::DirectArenaCandidate &&
            plan.reason == V2ReplayReason::Eligible &&
            plan.logicalSource && plan.requiresAdmission() && plan.layout,
        "one whole eligible raw chunk becomes one direct arena candidate");
  check(plan.capacity.commandHeaders == 6 &&
            plan.capacity.drawRunRecords == 1 &&
            plan.capacity.clearRecords == 1 &&
            plan.capacity.clearRects == 2 &&
            plan.capacity.surfaceCopyRecords == 1 &&
            plan.capacity.stretchRectRecords == 1 &&
            plan.capacity.colorFillRecords == 1 &&
            plan.capacity.depthResolveRecords == 1,
        "planner counts every eligible typed payload without semantic replay");
  check(plan.capacity.drawUniformVertexConstantBytes ==
                sizeof(dxmt9::core::VertexShaderConstants) &&
            plan.capacity.drawUniformPixelConstantBytes ==
                sizeof(dxmt9::core::PixelShaderConstants),
        "every draw reserves a complete persistent VS/PS constant snapshot");
  check(plan.capacity.drawUniformPayloadLookupHeads == 8 &&
            plan.capacity.drawUniformPayloadLookupTails == 8 &&
            plan.capacity.drawUniformPayloadLookupNext == 1 &&
            plan.capacity.drawUniformVertexConstantsLookupHeads == 8 &&
            plan.capacity.drawUniformVertexConstantsLookupTails == 8 &&
            plan.capacity.drawUniformVertexConstantsLookupNext == 1 &&
            plan.capacity.drawUniformPixelConstantsLookupHeads == 8 &&
            plan.capacity.drawUniformPixelConstantsLookupTails == 8 &&
            plan.capacity.drawUniformPixelConstantsLookupNext == 1,
        "one draw uses eight lookup buckets and one next link per group");
}

void stateOnlyRawHasNoLogicalSourceOrAdmission() {
  const std::array records{
      drawRecord(D9C_COMMAND_RECORD_APPLY_STATE),
  };
  const auto fixture = makeValidatedFixture(records);
  const auto plan = dxmt9::d3d9::planCpuReadyChunkV2(fixture.view(), 9);
  check(plan.lane == V2ReplayLane::StateOnly &&
            plan.reason == V2ReplayReason::Eligible &&
            !plan.logicalSource && !plan.layout &&
            !plan.requiresAdmission() &&
            plan.replaysSemanticsExactlyOnce(),
        "state-only raw replays exactly once without source admission");
}

RecordSpec blockingRecord(std::uint32_t type) {
  switch (type) {
  case D9C_COMMAND_RECORD_QUERY_ISSUE:
    return {
        .type = type,
        .payload = bytesOf(D9CCommandChunkWireQueryIssueV2{
            .queryHandleIndex = 0}),
        .handles = {handle(D9C_CHUNK_HANDLE_KIND_QUERY, 10)},
    };
  case D9C_COMMAND_RECORD_READBACK:
    return {
        .type = type,
        .payload = bytesOf(D9CCommandChunkWireReadbackV2{0, 1}),
        .handles = {handle(D9C_CHUNK_HANDLE_KIND_SURFACE, 11),
                    handle(D9C_CHUNK_HANDLE_KIND_SURFACE, 12)},
    };
  case D9C_COMMAND_RECORD_UPDATE_TEXTURE:
    return {
        .type = type,
        .payload = bytesOf(D9CCommandChunkWireUpdateTextureV2{0, 1}),
        .handles = {handle(D9C_CHUNK_HANDLE_KIND_TEXTURE, 13),
                    handle(D9C_CHUNK_HANDLE_KIND_TEXTURE, 14)},
    };
  case D9C_COMMAND_RECORD_PRESENT:
    return {.type = type,
            .payload = bytesOf(D9CCommandChunkWirePresentV2{})};
  default:
    return {};
  }
}

void blockersSelectOneWholeRawFallbackLane() {
  struct Expected {
    std::uint32_t type;
    V2ReplayLane lane;
    V2ReplayReason reason;
  };
  const std::array expected{
      Expected{D9C_COMMAND_RECORD_QUERY_ISSUE,
               V2ReplayLane::Legacy, V2ReplayReason::Query},
      Expected{D9C_COMMAND_RECORD_READBACK,
               V2ReplayLane::Inline, V2ReplayReason::Readback},
      Expected{D9C_COMMAND_RECORD_UPDATE_TEXTURE,
               V2ReplayLane::Legacy, V2ReplayReason::UpdateTexture},
  };
  for (const auto& item : expected) {
    const std::array records{drawRecord(), blockingRecord(item.type)};
    const auto fixture = makeValidatedFixture(records);
    const auto plan = dxmt9::d3d9::planCpuReadyChunkV2(fixture.view(), 5);
    check(plan.lane == item.lane && plan.reason == item.reason &&
              !plan.requiresAdmission(),
          "one blocker must route the complete raw chunk off the arena lane");
  }

  const D9CCommandChunkWireRecordHeaderV2 unknownHeader{
      .type = 0xffff,
  };
  const ImportedChunkV2View unknown{
      .header = {.recordCount = 1},
      .records = std::span(&unknownHeader, 1),
  };
  const auto unknownPlan =
      dxmt9::d3d9::planCpuReadyChunkV2(unknown, 6);
  check(unknownPlan.lane == V2ReplayLane::Reject &&
            unknownPlan.reason == V2ReplayReason::UnknownRecord &&
            !unknownPlan.replaysSemanticsExactlyOnce(),
        "defensive unknown records reject rather than replay through legacy");
}

void partialConstantDeltasDoNotInflateSnapshotCapacity() {
  std::vector<RecordSpec> records;
  for (std::uint32_t i = 0; i < 16; ++i) {
    records.push_back(partialFloatConstantRecord(
        D9C_COMMAND_RECORD_SET_VS_CONST_F, i));
    records.push_back(partialFloatConstantRecord(
        D9C_COMMAND_RECORD_SET_PS_CONST_F, i));
  }
  records.push_back(drawRecord());
  const auto fixture = makeValidatedFixture(records);
  const auto plan =
      dxmt9::d3d9::planCpuReadyChunkV2(fixture.view(), 15);
  check(plan.directArenaCandidate() &&
            plan.capacity.drawUniformVertexConstantBytes ==
                sizeof(dxmt9::core::VertexShaderConstants) &&
            plan.capacity.drawUniformPixelConstantBytes ==
                sizeof(dxmt9::core::PixelShaderConstants),
        "partial SET_CONST deltas do not exceed one full snapshot per draw");
}

void triangleFanAndLookupBoundaryPlans() {
  struct FanForm {
    std::uint32_t type;
    std::size_t minimumGeneratedBytes;
  };
  const std::array fanForms{
      FanForm{D9C_COMMAND_RECORD_DRAW_PRIMITIVE, 3u * 3u * 2u},
      FanForm{D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE, 3u * 3u * 4u},
      FanForm{D9C_COMMAND_RECORD_DRAW_PRIMITIVE_UP, 3u * 3u * 4u},
      FanForm{D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE_UP,
              (3u + 2u) * 4u + 3u * 3u * 2u},
  };
  for (const auto& form : fanForms) {
    const std::array fanRecords{triangleFanRecord(form.type)};
    const auto fanFixture = makeValidatedFixture(fanRecords);
    const auto fanPlan =
        dxmt9::d3d9::planCpuReadyChunkV2(fanFixture.view(), 10);
    check(fanPlan.directArenaCandidate() && fanPlan.segmentCount == 1 &&
              fanPlan.capacity.drawPayloadBytes >=
                  form.minimumGeneratedBytes,
          "every TriangleFan wire form plans its worst-case transformed payload");
  }

  std::vector<RecordSpec> fiveDraws(5, drawRecord());
  const auto fiveFixture = makeValidatedFixture(fiveDraws);
  const auto plan =
      dxmt9::d3d9::planCpuReadyChunkV2(fiveFixture.view(), 11);
  check(plan.directArenaCandidate() &&
            plan.capacity.drawUniformPayloadLookupHeads == 16 &&
            plan.capacity.drawUniformPayloadLookupTails == 16 &&
            plan.capacity.drawUniformPayloadLookupNext == 5 &&
            plan.capacity.drawUniformVertexConstantsLookupHeads == 16 &&
            plan.capacity.drawUniformPixelConstantsLookupHeads == 16 &&
            plan.capacity.drawUniformVertexConstantBytes ==
                5 * sizeof(dxmt9::core::VertexShaderConstants) &&
            plan.capacity.drawUniformPixelConstantBytes ==
                5 * sizeof(dxmt9::core::PixelShaderConstants),
        "lookup buckets follow the minimum-eight power-of-two boundary");
}

void upPayloadUsesCheckedAlignedBuilderUpperBound() {
  const std::array records{directUpDrawRecord()};
  const auto fixture = makeValidatedFixture(records);
  const auto plan =
      dxmt9::d3d9::planCpuReadyChunkV2(fixture.view(), 14);
  const auto alignment = dxmt9::d3d9::kV2ArenaDrawPayloadAppendAlignment;
  std::size_t expected = 12;
  expected = alignUp(expected, alignment) +
             sizeof(dxmt9::core::DrawBindingOverride);
  expected = alignUp(expected, alignment) +
             sizeof(dxmt9::core::DrawBindingSnapshot);
  check(plan.directArenaCandidate() &&
            plan.capacity.drawPayloadBytes == expected &&
            plan.capacity.drawPayloadBytes <=
                std::numeric_limits<std::uint32_t>::max(),
        "UP and synthesized binding payloads share the checked aligned append policy");
}

void segmentedArenaBoundariesAndHardCaps() {
  const auto exact64Rects = clearRectCountForSegmentPages(64, true);
  const auto exact65Rects = clearRectCountForSegmentPages(65);
  const auto exact40Rects = clearRectCountForSegmentPages(40, true);

  const std::array exact64Records{clearRecord(exact64Rects)};
  const auto exact64Fixture = makeValidatedFixture(exact64Records);
  const auto exact64 = dxmt9::d3d9::planCpuReadyChunkV2(
      exact64Fixture.view(), 20,
      {.pageSize = 4096, .maxOrdinaryPagesPerSegment = 64});
  check(exact64.directArenaCandidate() && exact64.segmentCount == 1 &&
            exact64.segments[0].layout.pageCount == 64 &&
            !exact64.segments[0].jumbo && exact64.layout.has_value() &&
            exact64.arenaLayout->segmentCount == 1,
        "an ordinary block accepts the exact 64-page boundary");

  const std::array jumboRecords{clearRecord(exact65Rects)};
  const auto jumboFixture = makeValidatedFixture(jumboRecords);
  const auto jumbo = dxmt9::d3d9::planCpuReadyChunkV2(
      jumboFixture.view(), 21,
      {.pageSize = 4096,
       .maxOrdinaryPagesPerSegment = 64,
       .maxPagesPerSource = 128});
  check(jumbo.directArenaCandidate() && jumbo.segmentCount == 1 &&
            jumbo.segments[0].layout.pageCount == 65 &&
            jumbo.segments[0].jumbo,
        "one indivisible 65-page record receives one bounded jumbo block");
  const auto jumboCapped = dxmt9::d3d9::planCpuReadyChunkV2(
      jumboFixture.view(), 21,
      {.pageSize = 4096,
       .maxOrdinaryPagesPerSegment = 64,
       .maxPagesPerSource = 64});
  check(jumboCapped.lane == V2ReplayLane::Legacy &&
            jumboCapped.reason == V2ReplayReason::Oversize,
        "jumbo cannot exceed the complete source page cap");

  const std::array twoJumboRecords{
      clearRecord(exact65Rects), clearRecord(exact65Rects)};
  const auto twoJumboFixture = makeValidatedFixture(twoJumboRecords);
  const auto twoJumbo = dxmt9::d3d9::planCpuReadyChunkV2(
      twoJumboFixture.view(), 27,
      {.pageSize = 4096,
       .maxOrdinaryPagesPerSegment = 64,
       .maxSegmentsPerSource = 2,
       .maxPagesPerSource = 192});
  check(twoJumbo.directArenaCandidate() && twoJumbo.segmentCount == 2 &&
            twoJumbo.segments[0].jumbo &&
            twoJumbo.segments[1].jumbo &&
            twoJumbo.segments[0].firstRecordIndex == 0 &&
            twoJumbo.segments[0].recordCount == 1 &&
            twoJumbo.segments[1].firstRecordIndex == 1 &&
            twoJumbo.segments[1].recordCount == 1,
        "multiple indivisible records receive dedicated jumbo segments within source caps");

  const std::array splitRecords{
      clearRecord(exact40Rects), clearRecord(exact40Rects)};
  const auto splitFixture = makeValidatedFixture(splitRecords);
  const auto split = dxmt9::d3d9::planCpuReadyChunkV2(
      splitFixture.view(), 22,
      {.pageSize = 4096,
       .maxOrdinaryPagesPerSegment = 64,
       .maxSegmentsPerSource = 2,
       .maxPagesPerSource = 128});
  check(split.directArenaCandidate() && split.segmentCount == 2 &&
            !split.layout && split.arenaLayout->segmentCount == 2 &&
            split.segments[0].firstRecordIndex == 0 &&
            split.segments[0].recordCount == 1 &&
            split.segments[1].firstRecordIndex == 1 &&
            split.segments[1].recordCount == 1 &&
            split.segments[0].layout.pageCount <= 64 &&
            split.segments[1].layout.pageCount <= 64,
        "a 65+-page aggregate splits before the second GPU record in source order");
  const auto segmentCapped = dxmt9::d3d9::planCpuReadyChunkV2(
      splitFixture.view(), 22,
      {.pageSize = 4096,
       .maxOrdinaryPagesPerSegment = 64,
       .maxSegmentsPerSource = 1,
       .maxPagesPerSource = 128});
  check(segmentCapped.lane == V2ReplayLane::Legacy &&
            segmentCapped.reason == V2ReplayReason::Oversize,
        "the segment hard cap rejects a required second block");
  const auto pageCapped = dxmt9::d3d9::planCpuReadyChunkV2(
      splitFixture.view(), 22,
      {.pageSize = 4096,
       .maxOrdinaryPagesPerSegment = 64,
       .maxSegmentsPerSource = 2,
       .maxPagesPerSource = split.arenaLayout->pageCount - 1});
  check(pageCapped.lane == V2ReplayLane::Legacy &&
            pageCapped.reason == V2ReplayReason::Oversize,
        "packed segments still obey the complete source page hard cap");
}

void segmentedRangesCoverEveryRawRecordExactlyOnce() {
  const auto exact40Rects = clearRectCountForSegmentPages(40, true);
  const std::array records{
      drawRecord(D9C_COMMAND_RECORD_APPLY_STATE),
      clearRecord(exact40Rects),
      drawRecord(D9C_COMMAND_RECORD_APPLY_STATE),
      clearRecord(exact40Rects),
      drawRecord(D9C_COMMAND_RECORD_APPLY_STATE),
  };
  const auto fixture = makeValidatedFixture(records);
  const auto plan = dxmt9::d3d9::planCpuReadyChunkV2(
      fixture.view(), 24,
      {.pageSize = 4096,
       .maxOrdinaryPagesPerSegment = 64,
       .maxSegmentsPerSource = 2,
       .maxPagesPerSource = 128});
  check(plan.directArenaCandidate() && plan.segmentCount == 2 &&
            plan.segments[0].firstRecordIndex == 0 &&
            plan.segments[0].recordCount == 2 &&
            plan.segments[1].firstRecordIndex == 2 &&
            plan.segments[1].recordCount == 3,
        "segment ranges preserve leading, interstitial, and trailing state records exactly once");
  check(plan.segments[0].firstRecordIndex +
                plan.segments[0].recordCount ==
            plan.segments[1].firstRecordIndex &&
            plan.segments[1].firstRecordIndex +
                    plan.segments[1].recordCount ==
                records.size(),
        "segment ranges form one gap-free non-overlapping raw-record partition");
}

void presentTailIsAOrderedDirectSegment() {
  const auto exact64Rects = clearRectCountForSegmentPages(64, true);
  const std::array records{
      clearRecord(exact64Rects),
      blockingRecord(D9C_COMMAND_RECORD_PRESENT),
  };
  const auto fixture = makeValidatedFixture(records);
  const auto plan = dxmt9::d3d9::planCpuReadyChunkV2(
      fixture.view(), 23,
      {.pageSize = 4096,
       .maxOrdinaryPagesPerSegment = 64,
       .maxSegmentsPerSource = 2,
       .maxPagesPerSource = 128});
  check(plan.directArenaCandidate() && plan.segmentCount == 2 &&
            plan.capacity.presentRecords == 1 &&
            plan.segments[1].firstRecordIndex == 1 &&
            plan.segments[1].recordCount == 1 &&
            plan.segments[1].capacity.presentRecords == 1 &&
            plan.segments[1].capacity.commandHeaders == 1,
        "Present is a structurally sized Direct record at the logical tail");
}

void nonFinalOrRepeatedPresentFallsBackAsOneSource() {
  const std::array presentBeforeDraw{
      blockingRecord(D9C_COMMAND_RECORD_PRESENT),
      drawRecord(),
  };
  const auto beforeDrawFixture = makeValidatedFixture(presentBeforeDraw);
  const auto beforeDraw = dxmt9::d3d9::planCpuReadyChunkV2(
      beforeDrawFixture.view(), 25);
  check(beforeDraw.lane == V2ReplayLane::Legacy &&
            beforeDraw.reason == V2ReplayReason::Present &&
            beforeDraw.replaysSemanticsExactlyOnce(),
        "Present before later work falls back as one ordered legacy source");

  const std::array repeatedPresent{
      blockingRecord(D9C_COMMAND_RECORD_PRESENT),
      blockingRecord(D9C_COMMAND_RECORD_PRESENT),
  };
  const auto repeatedFixture = makeValidatedFixture(repeatedPresent);
  const auto repeated = dxmt9::d3d9::planCpuReadyChunkV2(
      repeatedFixture.view(), 26);
  check(repeated.lane == V2ReplayLane::Legacy &&
            repeated.reason == V2ReplayReason::Present &&
            repeated.replaysSemanticsExactlyOnce(),
        "multiple Present records fall back as one ordered legacy source");
}

void oversizeAndOverflowFallbackBeforeReplay() {
  const auto fixture = makeValidatedFixture(eligibleRecords());
  const auto base =
      dxmt9::d3d9::planCpuReadyChunkV2(fixture.view(), 12);
  check(base.directArenaCandidate() && base.layout &&
            base.layout->pageCount > 1,
        "page-boundary fixture must span multiple pages");
  const auto exactBoundary = dxmt9::d3d9::planCpuReadyChunkV2(
      fixture.view(), 12,
      {.pageSize = 4096, .maxPages = base.layout->pageCount});
  const auto belowBoundary = dxmt9::d3d9::planCpuReadyChunkV2(
      fixture.view(), 12,
      {.pageSize = 4096, .maxPages = base.layout->pageCount - 1});
  check(exactBoundary.directArenaCandidate() &&
            belowBoundary.lane == V2ReplayLane::Legacy &&
            belowBoundary.reason == V2ReplayReason::Oversize,
        "maxPages accepts the exact boundary and rejects one page below it");
  const auto oversize = dxmt9::d3d9::planCpuReadyChunkV2(
      fixture.view(), 12,
      {.pageSize = dxmt9::core::kSourcePayloadByteAlignment,
       .maxPages = 1});
  check(oversize.lane == V2ReplayLane::Legacy &&
            oversize.reason == V2ReplayReason::Oversize &&
            !oversize.layout,
        "a valid plan beyond the configured page lane falls back pre-replay");

  std::array<D9CCommandChunkWireClearV2, 2> clears{{
      {.rectCount = std::numeric_limits<std::uint32_t>::max()},
      {.rectCount = 1},
  }};
  std::array<D9CCommandChunkWireRecordHeaderV2, 2> headers{};
  for (std::size_t i = 0; i < headers.size(); ++i) {
    headers[i] = {
        .type = D9C_COMMAND_RECORD_CLEAR,
        .payloadOffset = static_cast<std::uint32_t>(i * sizeof(clears[0])),
        .payloadSize = sizeof(clears[0]),
    };
  }
  const ImportedChunkV2View arithmeticFixture{
      .header = {.recordCount = 2},
      .records = headers,
      .payloadArena = std::span<const std::byte>(
          reinterpret_cast<const std::byte*>(clears.data()), sizeof(clears)),
  };
  const auto overflow =
      dxmt9::d3d9::planCpuReadyChunkV2(arithmeticFixture, 13);
  check(overflow.lane == V2ReplayLane::Reject &&
            overflow.reason == V2ReplayReason::StructuralOverflow &&
            !overflow.layout && !overflow.replaysSemanticsExactlyOnce(),
        "u32 capacity overflow rejects before semantic replay");

}

}  // namespace

int main() {
  try {
    eligibleWholeRawPlanHasTypedCapacity();
    stateOnlyRawHasNoLogicalSourceOrAdmission();
    blockersSelectOneWholeRawFallbackLane();
    partialConstantDeltasDoNotInflateSnapshotCapacity();
    triangleFanAndLookupBoundaryPlans();
    upPayloadUsesCheckedAlignedBuilderUpperBound();
    segmentedArenaBoundariesAndHardCaps();
    segmentedRangesCoverEveryRawRecordExactlyOnce();
    presentTailIsAOrderedDirectSegment();
    nonFinalOrRepeatedPresentFallsBackAsOneSource();
    oversizeAndOverflowFallbackBeforeReplay();
  } catch (const std::exception& error) {
    std::cerr << "cpu_ready_plan_spec failed: " << error.what() << '\n';
    return 1;
  }
  return 0;
}
