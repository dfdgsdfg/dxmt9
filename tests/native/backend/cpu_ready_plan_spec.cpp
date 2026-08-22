#include "device_c_cpu_ready_plan.hpp"
#include "device_c_ordered_control.hpp"

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
#include <type_traits>
#include <vector>

namespace {

using dxmt9::d3d9::ImportedChunkView;
using dxmt9::d3d9::OrderedControlDisposition;
using dxmt9::d3d9::OrderedControlKind;
using dxmt9::d3d9::RawCommandChunk;
using dxmt9::d3d9::CommandChunkEnvelope;
using dxmt9::d3d9::ReplayLane;
using dxmt9::d3d9::ReplayReason;
using dxmt9::core::metalqueue::SessionReleaseAction;

static_assert(std::is_trivially_copyable_v<OrderedControlDisposition>);
static_assert(std::is_standard_layout_v<OrderedControlDisposition>);

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
  std::vector<D9CCommandChunkWireHandleEntry> handles;
};

struct ValidatedFixture {
  std::vector<std::byte> bytes;
  CommandChunkEnvelope envelope{};

  ImportedChunkView view() const {
    ImportedChunkView imported;
    const auto result = dxmt9::d3d9::validateCommandChunk(
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
  std::vector<D9CCommandChunkWireRecordHeader> records;
  std::vector<D9CCommandChunkWireHandleEntry> handles;
  std::vector<std::byte> payload;
  for (const auto& spec : specs) {
    const auto* rule = dxmt9::d3d9::recordRule(spec.type);
    check(rule != nullptr, "fixture record must exist in the canonical schema");
    payload.resize(alignUp(payload.size(), rule->payloadAlignment));
    records.push_back({
        .type = spec.type,
        .flags = D9C_COMMAND_CHUNK_RECORD_FLAG_NONE,
        .payloadOffset = static_cast<std::uint32_t>(payload.size()),
        .payloadSize = static_cast<std::uint32_t>(spec.payload.size()),
        .firstHandle = static_cast<std::uint32_t>(handles.size()),
        .handleCount = static_cast<std::uint32_t>(spec.handles.size()),
    });
    payload.insert(payload.end(), spec.payload.begin(), spec.payload.end());
    handles.insert(handles.end(), spec.handles.begin(), spec.handles.end());
  }

  D9CCommandChunkWireHeader header{
      .version = D9C_COMMAND_CHUNK_WIRE_VERSION,
      .headerSize = D9C_COMMAND_CHUNK_WIRE_HEADER_SIZE,
      .recordHeaderSize = D9C_COMMAND_CHUNK_WIRE_RECORD_HEADER_SIZE,
      .handleEntrySize = D9C_COMMAND_CHUNK_WIRE_HANDLE_ENTRY_SIZE,
      .recordTableOffset = D9C_COMMAND_CHUNK_WIRE_HEADER_SIZE,
      .recordCount = static_cast<std::uint32_t>(records.size()),
      .handleCount = static_cast<std::uint32_t>(handles.size()),
      .payloadArenaSize = static_cast<std::uint32_t>(payload.size()),
  };
  header.handleTableOffset = static_cast<std::uint32_t>(alignUp(
      header.recordTableOffset + records.size() * sizeof(records[0]),
      alignof(D9CCommandChunkWireHandleEntry)));
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
      .version = D9C_COMMAND_CHUNK_VERSION,
      .recordCount = header.recordCount,
      .handleCount = header.handleCount,
  };
  return fixture;
}

D9CCommandChunkWireHandleEntry handle(std::uint32_t kind,
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
  D9CCommandChunkWireDrawHeader draw{};
  if (type != D9C_COMMAND_RECORD_APPLY_STATE) {
    draw.primitiveType = primitiveType;
    draw.startVertex = 3;
    draw.primitiveCount = 1;
  }
  draw.sectionTableOffset = sizeof(D9CCommandChunkWireDrawHeader);
  draw.sectionPayloadOffset = sizeof(D9CCommandChunkWireDrawHeader);
  return {.type = type, .payload = bytesOf(draw)};
}

RecordSpec directUpDrawRecord() {
  constexpr std::size_t kVertexBytes = 12;
  D9CCommandChunkWireDrawHeader draw{
      .primitiveType = 4,
      .primitiveCount = 1,
      .stride = 4,
      .sectionCount = 1,
      .sectionTableOffset = sizeof(D9CCommandChunkWireDrawHeader),
  };
  draw.sectionPayloadOffset = static_cast<std::uint32_t>(alignUp(
      sizeof(draw) + sizeof(D9CCommandChunkWireSectionDesc),
      alignof(std::uint32_t)));
  const auto* rule = dxmt9::d3d9::sectionRule(
      D9C_COMMAND_CHUNK_SECTION_UP_VERTEX_DATA);
  check(rule != nullptr, "UP vertex section rule must exist");
  const D9CCommandChunkWireSectionDesc section{
      .kind = D9C_COMMAND_CHUNK_SECTION_UP_VERTEX_DATA,
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
  D9CCommandChunkWireDrawHeader draw{
      .primitiveType = 6,
      .primitiveCount = primitiveCount,
      .sectionTableOffset = sizeof(D9CCommandChunkWireDrawHeader),
  };
  std::array<D9CCommandChunkWireSectionDesc, 2> sections{};
  std::size_t sectionCount = 0;
  std::array<std::size_t, 2> sectionBytes{};
  if (type == D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE) {
    draw.numVertices = primitiveCount + 2u;
  } else if (type == D9C_COMMAND_RECORD_DRAW_PRIMITIVE_UP) {
    draw.stride = stride;
    sectionBytes[sectionCount] =
        static_cast<std::size_t>(primitiveCount + 2u) * stride;
    sections[sectionCount++].kind =
        D9C_COMMAND_CHUNK_SECTION_UP_VERTEX_DATA;
  } else if (type == D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE_UP) {
    draw.numVertices = primitiveCount + 2u;
    draw.stride = stride;
    draw.indexFormat = indexFormat;
    sectionBytes[sectionCount] =
        static_cast<std::size_t>(primitiveCount + 2u) *
        (indexFormat == 102u ? 4u : 2u);
    sections[sectionCount++].kind =
        D9C_COMMAND_CHUNK_SECTION_UP_INDEX_DATA;
    sectionBytes[sectionCount] =
        static_cast<std::size_t>(draw.numVertices) * stride;
    sections[sectionCount++].kind =
        D9C_COMMAND_CHUNK_SECTION_UP_VERTEX_DATA;
  }
  draw.sectionCount = static_cast<std::uint32_t>(sectionCount);
  draw.sectionPayloadOffset = static_cast<std::uint32_t>(alignUp(
      sizeof(draw) + sectionCount * sizeof(sections[0]),
      alignof(std::uint32_t)));
  std::size_t payloadEnd = draw.sectionPayloadOffset;
  for (std::size_t i = 0; i < sectionCount; ++i) {
    const auto* rule = dxmt9::d3d9::sectionRule(sections[i].kind);
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
  D9CCommandChunkWireClear clear{
      .rectCount = static_cast<std::uint32_t>(rectCount),
      .rectOffset = sizeof(D9CCommandChunkWireClear),
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
  const D9CCommandChunkWireSetConst fixed{
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
          .payload = bytesOf(D9CCommandChunkWireUpdateSurface{
              .srcHandleIndex = 0, .dstHandleIndex = 1}),
          .handles = {handle(D9C_CHUNK_HANDLE_KIND_SURFACE, 1),
                      handle(D9C_CHUNK_HANDLE_KIND_SURFACE, 2)},
      },
      RecordSpec{
          .type = D9C_COMMAND_RECORD_STRETCH_RECT,
          .payload = bytesOf(D9CCommandChunkWireStretchRect{
              .srcHandleIndex = 2, .dstHandleIndex = 3}),
          .handles = {handle(D9C_CHUNK_HANDLE_KIND_SURFACE, 3),
                      handle(D9C_CHUNK_HANDLE_KIND_SURFACE, 4)},
      },
      RecordSpec{
          .type = D9C_COMMAND_RECORD_COLOR_FILL,
          .payload = bytesOf(D9CCommandChunkWireColorFill{
              .surfaceHandleIndex = 4}),
          .handles = {handle(D9C_CHUNK_HANDLE_KIND_SURFACE, 5)},
      },
      RecordSpec{
          .type = D9C_COMMAND_RECORD_RESZ_DEPTH_RESOLVE,
          .payload = bytesOf(D9CCommandChunkWireReszDepthResolve{
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
  const auto plan = dxmt9::d3d9::planCpuReadyChunk(
      imported, raw.replaySeq);
  check(plan.rawOrdinal == raw.replaySeq &&
            plan.lane == ReplayLane::DirectArenaCandidate &&
            plan.reason == ReplayReason::Eligible &&
            plan.logicalSource && plan.requiresAdmission() && plan.layout,
        "one whole eligible raw chunk becomes one direct arena candidate");
  check(!plan.containsOrderedControls,
        "an eligible Arena plan has no compatibility disposition");
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
  const auto plan = dxmt9::d3d9::planCpuReadyChunk(fixture.view(), 9);
  check(plan.lane == ReplayLane::StateOnly &&
            plan.reason == ReplayReason::Eligible &&
            !plan.logicalSource && !plan.layout &&
            !plan.containsOrderedControls &&
            !plan.requiresAdmission() &&
            plan.replaysSemanticsExactlyOnce(),
        "state-only raw replays exactly once without source admission");
}

RecordSpec blockingRecord(std::uint32_t type,
                          std::uint32_t firstHandle = 0) {
  switch (type) {
  case D9C_COMMAND_RECORD_QUERY_ISSUE:
    return {
        .type = type,
        .payload = bytesOf(D9CCommandChunkWireQueryIssue{
            .queryHandleIndex = firstHandle,
            .flags = 0x41u}),
        .handles = {handle(D9C_CHUNK_HANDLE_KIND_QUERY, 10)},
    };
  case D9C_COMMAND_RECORD_READBACK:
    return {
        .type = type,
        .payload = bytesOf(D9CCommandChunkWireReadback{
            firstHandle, firstHandle + 1u}),
        .handles = {handle(D9C_CHUNK_HANDLE_KIND_SURFACE, 11),
                    handle(D9C_CHUNK_HANDLE_KIND_SURFACE, 12)},
    };
  case D9C_COMMAND_RECORD_UPDATE_TEXTURE:
    return {
        .type = type,
        .payload = bytesOf(D9CCommandChunkWireUpdateTexture{
            firstHandle, firstHandle + 1u}),
        .handles = {handle(D9C_CHUNK_HANDLE_KIND_TEXTURE, 13),
                    handle(D9C_CHUNK_HANDLE_KIND_TEXTURE, 14)},
    };
  case D9C_COMMAND_RECORD_PRESENT:
    return {.type = type,
            .payload = bytesOf(D9CCommandChunkWirePresent{})};
  default:
    return {};
  }
}

void blockersSelectOneWholeRawFallbackLane() {
  struct Expected {
    std::uint32_t type;
    ReplayLane lane;
    ReplayReason reason;
    OrderedControlKind kind;
    SessionReleaseAction action;
    std::uint32_t handleCount;
  };
  const std::array expected{
      Expected{D9C_COMMAND_RECORD_QUERY_ISSUE,
               ReplayLane::Legacy, ReplayReason::Query,
               OrderedControlKind::Query,
               SessionReleaseAction::ClosePass, 1},
      Expected{D9C_COMMAND_RECORD_READBACK,
               ReplayLane::Inline, ReplayReason::Readback,
               OrderedControlKind::Readback,
               SessionReleaseAction::SubmitAndWait, 2},
      Expected{D9C_COMMAND_RECORD_UPDATE_TEXTURE,
               ReplayLane::Legacy, ReplayReason::UpdateTexture,
               OrderedControlKind::UpdateTexture,
               SessionReleaseAction::SubmitSession, 2},
  };
  for (const auto& item : expected) {
    const std::array records{
        drawRecord(), blockingRecord(item.type), drawRecord()};
    const auto fixture = makeValidatedFixture(records);
    const auto plan = dxmt9::d3d9::planCpuReadyChunk(fixture.view(), 5);
    check(plan.lane == item.lane && plan.reason == item.reason &&
              !plan.requiresAdmission() && plan.containsOrderedControls,
          "one blocker must route the complete raw chunk off the arena lane");
    const auto control = dxmt9::d3d9::makeOrderedControlDisposition(
        fixture.view().record(1), 5, 1);
    check(control && control->valid(),
          "the exact control descriptor rebuilds from the validated raw");
    check(control->kind == item.kind &&
              control->requiredReleaseAction == item.action &&
              control->rawOrdinal == 5 && control->recordIndex == 1 &&
              control->recordType == item.type &&
              control->firstHandle == 0 &&
              control->handleCount == item.handleCount &&
              control->primaryHandleIndex == 0,
          "the validated raw rebuilds the control's exact order and locator");
    check((item.handleCount == 1 &&
               control->secondaryHandleIndex ==
                   dxmt9::d3d9::kNoOrderedControlHandleIndex &&
               control->controlFlags == 0x41u) ||
              (item.handleCount == 2 &&
               control->secondaryHandleIndex == 1 &&
               control->controlFlags == 0),
          "control-specific locator fields preserve the validated wire form");
  }

  const std::array multipleBlockers{
      drawRecord(),
      blockingRecord(D9C_COMMAND_RECORD_QUERY_ISSUE),
      blockingRecord(D9C_COMMAND_RECORD_READBACK, 1),
      drawRecord(),
  };
  const auto multipleFixture = makeValidatedFixture(multipleBlockers);
  const auto multiple = dxmt9::d3d9::planCpuReadyChunk(
      multipleFixture.view(), 17);
  check(multiple.containsOrderedControls &&
            multiple.lane == ReplayLane::Inline &&
            multiple.reason == ReplayReason::Readback,
        "all controls preflight and any readback selects whole-raw Inline");
  auto invalid = *dxmt9::d3d9::makeOrderedControlDisposition(
      multipleFixture.view().record(1), 17, 1);
  invalid.requiredReleaseAction = SessionReleaseAction::SubmitSession;
  check(!OrderedControlDisposition{}.valid() && !invalid.valid(),
        "default and action-mismatched dispositions are structurally invalid");

  const D9CCommandChunkWireRecordHeader unknownHeader{
      .type = 0xffff,
  };
  const ImportedChunkView unknown{
      .header = {.recordCount = 1},
      .records = std::span(&unknownHeader, 1),
  };
  const auto unknownPlan =
      dxmt9::d3d9::planCpuReadyChunk(unknown, 6);
  check(unknownPlan.lane == ReplayLane::Reject &&
            unknownPlan.reason == ReplayReason::UnknownRecord &&
            !unknownPlan.containsOrderedControls &&
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
      dxmt9::d3d9::planCpuReadyChunk(fixture.view(), 15);
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
        dxmt9::d3d9::planCpuReadyChunk(fanFixture.view(), 10);
    check(fanPlan.directArenaCandidate() && fanPlan.segmentCount == 1 &&
              fanPlan.capacity.drawPayloadBytes >=
                  form.minimumGeneratedBytes,
          "every TriangleFan wire form plans its worst-case transformed payload");
  }

  std::vector<RecordSpec> fiveDraws(5, drawRecord());
  const auto fiveFixture = makeValidatedFixture(fiveDraws);
  const auto plan =
      dxmt9::d3d9::planCpuReadyChunk(fiveFixture.view(), 11);
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
      dxmt9::d3d9::planCpuReadyChunk(fixture.view(), 14);
  const auto alignment = dxmt9::d3d9::kArenaDrawPayloadAppendAlignment;
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
  const auto exact64 = dxmt9::d3d9::planCpuReadyChunk(
      exact64Fixture.view(), 20,
      {.pageSize = 4096, .maxOrdinaryPagesPerSegment = 64});
  check(exact64.directArenaCandidate() && exact64.segmentCount == 1 &&
            exact64.segments[0].layout.pageCount == 64 &&
            !exact64.segments[0].jumbo && exact64.layout.has_value() &&
            exact64.arenaLayout->segmentCount == 1,
        "an ordinary block accepts the exact 64-page boundary");

  const std::array jumboRecords{clearRecord(exact65Rects)};
  const auto jumboFixture = makeValidatedFixture(jumboRecords);
  const auto jumbo = dxmt9::d3d9::planCpuReadyChunk(
      jumboFixture.view(), 21,
      {.pageSize = 4096,
       .maxOrdinaryPagesPerSegment = 64,
       .maxPagesPerSource = 128});
  check(jumbo.directArenaCandidate() && jumbo.segmentCount == 1 &&
            jumbo.segments[0].layout.pageCount == 65 &&
            jumbo.segments[0].jumbo,
        "one indivisible 65-page record receives one bounded jumbo block");
  const auto jumboCapped = dxmt9::d3d9::planCpuReadyChunk(
      jumboFixture.view(), 21,
      {.pageSize = 4096,
       .maxOrdinaryPagesPerSegment = 64,
       .maxPagesPerSource = 64});
  check(jumboCapped.lane == ReplayLane::Legacy &&
            jumboCapped.reason == ReplayReason::Oversize,
        "jumbo cannot exceed the complete source page cap");

  const std::array twoJumboRecords{
      clearRecord(exact65Rects), clearRecord(exact65Rects)};
  const auto twoJumboFixture = makeValidatedFixture(twoJumboRecords);
  const auto twoJumbo = dxmt9::d3d9::planCpuReadyChunk(
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
  const auto split = dxmt9::d3d9::planCpuReadyChunk(
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
  const auto segmentCapped = dxmt9::d3d9::planCpuReadyChunk(
      splitFixture.view(), 22,
      {.pageSize = 4096,
       .maxOrdinaryPagesPerSegment = 64,
       .maxSegmentsPerSource = 1,
       .maxPagesPerSource = 128});
  check(segmentCapped.lane == ReplayLane::Legacy &&
            segmentCapped.reason == ReplayReason::Oversize,
        "the segment hard cap rejects a required second block");
  const auto pageCapped = dxmt9::d3d9::planCpuReadyChunk(
      splitFixture.view(), 22,
      {.pageSize = 4096,
       .maxOrdinaryPagesPerSegment = 64,
       .maxSegmentsPerSource = 2,
       .maxPagesPerSource = split.arenaLayout->pageCount - 1});
  check(pageCapped.lane == ReplayLane::Legacy &&
            pageCapped.reason == ReplayReason::Oversize,
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
  const auto plan = dxmt9::d3d9::planCpuReadyChunk(
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

void segmentedSourcesPreserveRawRangesAndTailOwnership() {
  const auto exact40Rects = clearRectCountForSegmentPages(40, true);
  const std::array records{
      drawRecord(D9C_COMMAND_RECORD_APPLY_STATE),
      clearRecord(exact40Rects),
      drawRecord(D9C_COMMAND_RECORD_APPLY_STATE),
      clearRecord(exact40Rects),
      drawRecord(D9C_COMMAND_RECORD_APPLY_STATE),
  };
  const auto fixture = makeValidatedFixture(records);
  const auto plan = dxmt9::d3d9::planCpuReadyChunk(
      fixture.view(), 241,
      {.pageSize = 4096,
       .maxOrdinaryPagesPerSegment = 64,
       .maxSegmentsPerSource = 1,
       .maxPagesPerSource = 64,
       .maxSourcesPerChunk = 2});
  check(plan.directArenaCandidate() && plan.sourceCount == 2 &&
            !plan.arenaLayout.has_value() && plan.segmentCount == 2,
        "explicit source segmentation keeps the physical blocks bounded and exposes two sources");
  for (std::size_t i = 0; i < plan.sourceCount; ++i) {
    const auto& source = plan.sources[i];
    check(source.valid() && source.segmentCount == 1 &&
              source.firstSegmentIndex == i && source.arenaLayout.pageCount <= 64,
          "each segmented source owns one complete bounded Arena layout");
  }
  check(plan.sources[0].firstRecordIndex == 0u &&
            plan.sources[0].recordCount == 2u &&
            plan.sources[1].firstRecordIndex == 2u &&
            plan.sources[1].recordCount == 3u &&
            plan.sources[0].firstRecordIndex + plan.sources[0].recordCount ==
                plan.sources[1].firstRecordIndex &&
            plan.sources[1].firstRecordIndex + plan.sources[1].recordCount ==
                records.size(),
        "source ranges preserve the state prefix, interstitial state, and trailing state exactly once");
}

void segmentedPresentTailStaysInFinalSource() {
  const auto exact64Rects = clearRectCountForSegmentPages(64, true);
  const std::array records{
      clearRecord(exact64Rects),
      blockingRecord(D9C_COMMAND_RECORD_PRESENT),
  };
  const auto fixture = makeValidatedFixture(records);
  const auto plan = dxmt9::d3d9::planCpuReadyChunk(
      fixture.view(), 242,
      {.pageSize = 4096,
       .maxOrdinaryPagesPerSegment = 64,
       .maxSegmentsPerSource = 1,
       .maxPagesPerSource = 64,
       .maxSourcesPerChunk = 2});
  check(plan.directArenaCandidate() && plan.sourceCount == 2 &&
            plan.sources[0].firstRecordIndex == 0u &&
            plan.sources[0].recordCount == 1u &&
            plan.sources[1].firstRecordIndex == 1u &&
            plan.sources[1].recordCount == 1u &&
            plan.sources[1].arenaLayout.segmentCount == 1 &&
            plan.segments[1].capacity.presentRecords == 1u,
        "Present remains a final one-record source after source segmentation");
}

void sourcePlansScalePastPhysicalEightBlockCompatibilityBound() {
  const auto exact64Rects = clearRectCountForSegmentPages(64, true);
  std::vector<RecordSpec> records;
  records.reserve(9);
  for (std::size_t i = 0; i < 9; ++i) {
    records.push_back(clearRecord(exact64Rects));
  }
  const auto fixture = makeValidatedFixture(records);
  const auto plan = dxmt9::d3d9::planCpuReadyChunk(
      fixture.view(), 243,
      {.pageSize = 4096,
       .maxOrdinaryPagesPerSegment = 64,
       .maxSegmentsPerSource = 8,
       .maxPagesPerSource = 512,
       .maxSourcesPerChunk = 2});
  check(plan.directArenaCandidate() && plan.segmentCount == 9 &&
            plan.sourceCount == 2 && !plan.arenaLayout.has_value(),
        "nine physical segments now split into bounded sources instead of hitting the old eight-segment cap");
  check(plan.sources[0].segmentCount == 8 &&
            plan.sources[0].arenaLayout.pageCount <= 512 &&
            plan.sources[1].segmentCount == 1 &&
            plan.sources[1].firstSegmentIndex == 8u &&
            plan.sources[1].firstRecordIndex == 8u &&
            plan.sources[1].recordCount == 1u,
        "the first source accepts the complete 512-page grouping and the ninth block becomes a successor source");
}

void presentTailIsAOrderedDirectSegment() {
  const auto exact64Rects = clearRectCountForSegmentPages(64, true);
  const std::array records{
      clearRecord(exact64Rects),
      blockingRecord(D9C_COMMAND_RECORD_PRESENT),
  };
  const auto fixture = makeValidatedFixture(records);
  const auto plan = dxmt9::d3d9::planCpuReadyChunk(
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
  const auto beforeDraw = dxmt9::d3d9::planCpuReadyChunk(
      beforeDrawFixture.view(), 25);
  check(beforeDraw.lane == ReplayLane::Legacy &&
            beforeDraw.reason == ReplayReason::Present &&
            !beforeDraw.containsOrderedControls &&
            beforeDraw.replaysSemanticsExactlyOnce(),
        "Present before later work falls back as one ordered legacy source");

  const std::array repeatedPresent{
      blockingRecord(D9C_COMMAND_RECORD_PRESENT),
      blockingRecord(D9C_COMMAND_RECORD_PRESENT),
  };
  const auto repeatedFixture = makeValidatedFixture(repeatedPresent);
  const auto repeated = dxmt9::d3d9::planCpuReadyChunk(
      repeatedFixture.view(), 26);
  check(repeated.lane == ReplayLane::Legacy &&
            repeated.reason == ReplayReason::Present &&
            !repeated.containsOrderedControls &&
            repeated.replaysSemanticsExactlyOnce(),
        "multiple Present records fall back as one ordered legacy source");
}

void oversizeAndOverflowFallbackBeforeReplay() {
  const auto fixture = makeValidatedFixture(eligibleRecords());
  const auto base =
      dxmt9::d3d9::planCpuReadyChunk(fixture.view(), 12);
  check(base.directArenaCandidate() && base.layout &&
            base.layout->pageCount > 1,
        "page-boundary fixture must span multiple pages");
  const auto exactBoundary = dxmt9::d3d9::planCpuReadyChunk(
      fixture.view(), 12,
      {.pageSize = 4096, .maxPages = base.layout->pageCount});
  const auto belowBoundary = dxmt9::d3d9::planCpuReadyChunk(
      fixture.view(), 12,
      {.pageSize = 4096, .maxPages = base.layout->pageCount - 1});
  check(exactBoundary.directArenaCandidate() &&
            belowBoundary.lane == ReplayLane::Legacy &&
            belowBoundary.reason == ReplayReason::Oversize,
        "maxPages accepts the exact boundary and rejects one page below it");
  const auto oversize = dxmt9::d3d9::planCpuReadyChunk(
      fixture.view(), 12,
      {.pageSize = dxmt9::core::kSourcePayloadByteAlignment,
       .maxPages = 1});
  check(oversize.lane == ReplayLane::Legacy &&
            oversize.reason == ReplayReason::Oversize &&
            !oversize.layout && !oversize.containsOrderedControls,
        "a valid plan beyond the configured page lane falls back pre-replay");

  std::array<D9CCommandChunkWireClear, 2> clears{{
      {.rectCount = std::numeric_limits<std::uint32_t>::max()},
      {.rectCount = 1},
  }};
  std::array<D9CCommandChunkWireRecordHeader, 2> headers{};
  for (std::size_t i = 0; i < headers.size(); ++i) {
    headers[i] = {
        .type = D9C_COMMAND_RECORD_CLEAR,
        .payloadOffset = static_cast<std::uint32_t>(i * sizeof(clears[0])),
        .payloadSize = sizeof(clears[0]),
    };
  }
  const ImportedChunkView arithmeticFixture{
      .header = {.recordCount = 2},
      .records = headers,
      .payloadArena = std::span<const std::byte>(
          reinterpret_cast<const std::byte*>(clears.data()), sizeof(clears)),
  };
  const auto overflow =
      dxmt9::d3d9::planCpuReadyChunk(arithmeticFixture, 13);
  check(overflow.lane == ReplayLane::Reject &&
            overflow.reason == ReplayReason::StructuralOverflow &&
            !overflow.layout && !overflow.containsOrderedControls &&
            !overflow.replaysSemanticsExactlyOnce(),
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
    segmentedSourcesPreserveRawRangesAndTailOwnership();
    segmentedPresentTailStaysInFinalSource();
    sourcePlansScalePastPhysicalEightBlockCompatibilityBound();
    presentTailIsAOrderedDirectSegment();
    nonFinalOrRepeatedPresentFallsBackAsOneSource();
    oversizeAndOverflowFallbackBeforeReplay();
  } catch (const std::exception& error) {
    std::cerr << "cpu_ready_plan_spec failed: " << error.what() << '\n';
    return 1;
  }
  return 0;
}
