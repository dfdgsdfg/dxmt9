// Exhaustive truth table for the source-wide replay emission partition
// (R-BACK-2.90) and for the closed kRecordTopology table it is derived from.
//
// planReplayEmission is a pure predicate over an already-validated imported
// view, so the whole classifier domain is reachable without Wine, Metal, or a
// queue. Collapsing the 21 live record kinds to their five emission
// equivalence classes makes an exhaustive enumeration over every
// class-sequence of length 1..5 -- 3,905 cases -- both feasible and genuinely
// complete over the classifier's domain.

#include "device_c_cpu_ready_plan.hpp"
#include "device_c_ordered_control.hpp"
#include "device_c_record_utils.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace {

using dxmt9::d3d9::CommandChunkEnvelope;
using dxmt9::d3d9::EmissionLeaseBlock;
using dxmt9::d3d9::EmissionPlanReason;
using dxmt9::d3d9::EmissionSegmentKind;
using dxmt9::d3d9::ImportedChunkView;
using dxmt9::d3d9::devicec::ImportedRecordReplayCategory;
using dxmt9::d3d9::RecordReplayTopology;
using dxmt9::d3d9::ReplayEmissionPlan;

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
    const auto result =
        dxmt9::d3d9::validateCommandChunk(bytes, envelope, &imported);
    if (!result.valid()) {
      throw TestFailure(
          "emission fixture validation status " +
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

// ---------------------------------------------------------------------------
// The five emission equivalence classes.

enum class Klass : std::uint8_t {
  IslandDraw = 0,   // DRAW_PRIMITIVE, non-fan
  StateOnly,        // APPLY_STATE
  Coordinator,      // CLEAR
  OrderedControl,   // QUERY_ISSUE
  Compatibility,    // DRAW_PRIMITIVE_UP
  Count,
};

constexpr std::size_t kClassCount = static_cast<std::size_t>(Klass::Count);

char klassLetter(Klass klass) {
  switch (klass) {
  case Klass::IslandDraw: return 'D';
  case Klass::StateOnly: return 'S';
  case Klass::Coordinator: return 'C';
  case Klass::OrderedControl: return 'O';
  case Klass::Compatibility: return 'X';
  case Klass::Count: break;
  }
  return '?';
}

RecordSpec islandDrawRecord() {
  D9CCommandChunkWireDrawHeader draw{};
  draw.primitiveType = 4;  // TriangleList
  draw.startVertex = 3;
  draw.primitiveCount = 1;
  draw.sectionTableOffset = sizeof(D9CCommandChunkWireDrawHeader);
  draw.sectionPayloadOffset = sizeof(D9CCommandChunkWireDrawHeader);
  return {.type = D9C_COMMAND_RECORD_DRAW_PRIMITIVE,
          .payload = bytesOf(draw)};
}

RecordSpec applyStateRecord() {
  D9CCommandChunkWireDrawHeader draw{};
  draw.sectionTableOffset = sizeof(D9CCommandChunkWireDrawHeader);
  draw.sectionPayloadOffset = sizeof(D9CCommandChunkWireDrawHeader);
  return {.type = D9C_COMMAND_RECORD_APPLY_STATE, .payload = bytesOf(draw)};
}

RecordSpec clearRecord() {
  D9CCommandChunkWireClear clear{
      .rectCount = 0,
      .rectOffset = sizeof(D9CCommandChunkWireClear),
  };
  return {.type = D9C_COMMAND_RECORD_CLEAR, .payload = bytesOf(clear)};
}

// Handle indices in a record payload are chunk-global, so a control record's
// index depends on how many handles the records before it contributed.
RecordSpec queryIssueRecord(std::uint32_t handleBase = 0u) {
  return {
      .type = D9C_COMMAND_RECORD_QUERY_ISSUE,
      .payload = bytesOf(D9CCommandChunkWireQueryIssue{
          .queryHandleIndex = handleBase, .flags = 0x41u}),
      .handles = {D9CCommandChunkWireHandleEntry{
          .kind = D9C_CHUNK_HANDLE_KIND_QUERY,
          .generation = 1,
          .objectId = 10,
      }},
  };
}

RecordSpec upDrawRecord() {
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
  const auto* rule =
      dxmt9::d3d9::sectionRule(D9C_COMMAND_CHUNK_SECTION_UP_VERTEX_DATA);
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
  std::memcpy(payload.data() + draw.sectionTableOffset, &section,
              sizeof(section));
  return {.type = D9C_COMMAND_RECORD_DRAW_PRIMITIVE_UP,
          .payload = std::move(payload)};
}

RecordSpec recordForClass(Klass klass, std::uint32_t handleBase) {
  switch (klass) {
  case Klass::IslandDraw: return islandDrawRecord();
  case Klass::StateOnly: return applyStateRecord();
  case Klass::Coordinator: return clearRecord();
  case Klass::OrderedControl: return queryIssueRecord(handleBase);
  case Klass::Compatibility: return upDrawRecord();
  case Klass::Count: break;
  }
  throw TestFailure("unreachable emission class");
}

bool islandResidentClass(Klass klass) {
  return klass == Klass::IslandDraw || klass == Klass::StateOnly;
}

// ---------------------------------------------------------------------------
// T2 -- every live record kind gets exactly one whole-range class.
//
// The five-class enumeration above is exhaustive over the *emission* domain,
// but it uses one representative record per class. This table walks all 21
// live kinds through the cheap production gate so the alphabet itself is
// covered: `classifyDirectChunkSlotRange` must agree with kRecordTopology's
// islandResident row for every kind, and `planDirectChunkSlotRange` must be
// non-null exactly when the single-record range is island-eligible and holds
// a draw. Before kRecordTopology existed, four independent switches decided
// this and had already drifted on six kinds.

RecordSpec drawRecord(std::uint32_t type, std::uint32_t primitiveType) {
  D9CCommandChunkWireDrawHeader draw{};
  draw.primitiveType = primitiveType;
  draw.primitiveCount = 1;
  // The wire validator forbids startVertex on an indexed draw and startIndex
  // on a non-indexed one, so each kind carries only the cursor it owns.
  if (type == D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE) {
    draw.numVertices = 3;
    draw.startIndex = 0;
  } else {
    draw.startVertex = 3;
  }
  draw.sectionTableOffset = sizeof(D9CCommandChunkWireDrawHeader);
  draw.sectionPayloadOffset = sizeof(D9CCommandChunkWireDrawHeader);
  return {.type = type, .payload = bytesOf(draw)};
}

RecordSpec constantRecord(std::uint32_t type) {
  const std::uint32_t elementSize =
      (type == D9C_COMMAND_RECORD_SET_VS_CONST_B ||
       type == D9C_COMMAND_RECORD_SET_PS_CONST_B)
          ? 4u
          : 16u;
  const D9CCommandChunkWireSetConst fixed{.startRegister = 0u,
                                          .registerCount = 1u};
  std::vector<std::byte> payload(sizeof(fixed) + elementSize, std::byte{0});
  std::memcpy(payload.data(), &fixed, sizeof(fixed));
  return {.type = type, .payload = std::move(payload)};
}

D9CCommandChunkWireHandleEntry handleEntry(std::uint32_t kind,
                                           std::uint64_t objectId) {
  return D9CCommandChunkWireHandleEntry{
      .kind = kind, .generation = 1, .objectId = objectId};
}

RecordSpec indexedUpDrawRecord() {
  constexpr std::size_t kVertexBytes = 12;
  constexpr std::size_t kIndexCount = 3;
  D9CCommandChunkWireDrawHeader draw{
      .primitiveType = 4,
      .primitiveCount = 1,
      .stride = 4,
      .sectionCount = 2,
      .sectionTableOffset = sizeof(D9CCommandChunkWireDrawHeader),
  };
  draw.indexFormat = 101u;  // 16-bit UP index data
  draw.numVertices = 3;
  draw.sectionPayloadOffset = static_cast<std::uint32_t>(alignUp(
      sizeof(draw) + 2u * sizeof(D9CCommandChunkWireSectionDesc),
      alignof(std::uint32_t)));
  const auto* vertexRule =
      dxmt9::d3d9::sectionRule(D9C_COMMAND_CHUNK_SECTION_UP_VERTEX_DATA);
  const auto* indexRule =
      dxmt9::d3d9::sectionRule(D9C_COMMAND_CHUNK_SECTION_UP_INDEX_DATA);
  check(vertexRule != nullptr && indexRule != nullptr,
        "UP vertex and index section rules must exist");
  const std::uint32_t indexBytes =
      static_cast<std::uint32_t>(kIndexCount * sizeof(std::uint16_t));
  // Section descriptors must be strictly ascending by kind, and UP_INDEX (24)
  // precedes UP_VERTEX (25).
  const D9CCommandChunkWireSectionDesc indexSection{
      .kind = D9C_COMMAND_CHUNK_SECTION_UP_INDEX_DATA,
      .elementSize = indexRule->elementSize,
      .count = indexBytes,
      .payloadOffset = draw.sectionPayloadOffset,
      .byteSize = indexBytes,
  };
  const D9CCommandChunkWireSectionDesc vertexSection{
      .kind = D9C_COMMAND_CHUNK_SECTION_UP_VERTEX_DATA,
      .elementSize = vertexRule->elementSize,
      .count = kVertexBytes,
      .payloadOffset = static_cast<std::uint32_t>(
          alignUp(draw.sectionPayloadOffset + indexBytes,
                  vertexRule->payloadAlignment)),
      .byteSize = kVertexBytes,
  };
  std::vector<std::byte> payload(vertexSection.payloadOffset + kVertexBytes);
  std::memcpy(payload.data(), &draw, sizeof(draw));
  std::memcpy(payload.data() + draw.sectionTableOffset, &indexSection,
              sizeof(indexSection));
  std::memcpy(payload.data() + draw.sectionTableOffset + sizeof(indexSection),
              &vertexSection, sizeof(vertexSection));
  return {.type = D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE_UP,
          .payload = std::move(payload)};
}

RecordSpec recordForType(std::uint32_t type) {
  switch (type) {
  case D9C_COMMAND_RECORD_DRAW_PRIMITIVE:
  case D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE:
    return drawRecord(type, /*TriangleList=*/4u);
  case D9C_COMMAND_RECORD_DRAW_PRIMITIVE_UP:
    return upDrawRecord();
  case D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE_UP:
    return indexedUpDrawRecord();
  case D9C_COMMAND_RECORD_SET_VS_CONST_F:
  case D9C_COMMAND_RECORD_SET_VS_CONST_I:
  case D9C_COMMAND_RECORD_SET_VS_CONST_B:
  case D9C_COMMAND_RECORD_SET_PS_CONST_F:
  case D9C_COMMAND_RECORD_SET_PS_CONST_I:
  case D9C_COMMAND_RECORD_SET_PS_CONST_B:
    return constantRecord(type);
  case D9C_COMMAND_RECORD_APPLY_STATE:
    return applyStateRecord();
  case D9C_COMMAND_RECORD_CLEAR:
    return clearRecord();
  case D9C_COMMAND_RECORD_PRESENT:
    // handleCount == 0 is the legacy Present shape the validator accepts
    // with a zero sourceHandleIndex.
    return {.type = D9C_COMMAND_RECORD_PRESENT,
            .payload = bytesOf(D9CCommandChunkWirePresent{})};
  case D9C_COMMAND_RECORD_STRETCH_RECT:
    return {.type = D9C_COMMAND_RECORD_STRETCH_RECT,
            .payload = bytesOf(D9CCommandChunkWireStretchRect{
                .srcHandleIndex = 0u, .dstHandleIndex = 1u}),
            .handles = {handleEntry(D9C_CHUNK_HANDLE_KIND_SURFACE, 31),
                        handleEntry(D9C_CHUNK_HANDLE_KIND_SURFACE, 32)}};
  case D9C_COMMAND_RECORD_COLOR_FILL:
    return {.type = D9C_COMMAND_RECORD_COLOR_FILL,
            .payload = bytesOf(D9CCommandChunkWireColorFill{
                .surfaceHandleIndex = 0u}),
            .handles = {handleEntry(D9C_CHUNK_HANDLE_KIND_SURFACE, 33)}};
  case D9C_COMMAND_RECORD_UPDATE_TEXTURE:
    return {.type = D9C_COMMAND_RECORD_UPDATE_TEXTURE,
            .payload = bytesOf(D9CCommandChunkWireUpdateTexture{
                .srcHandleIndex = 0u, .dstHandleIndex = 1u}),
            .handles = {handleEntry(D9C_CHUNK_HANDLE_KIND_TEXTURE, 34),
                        handleEntry(D9C_CHUNK_HANDLE_KIND_TEXTURE, 35)}};
  case D9C_COMMAND_RECORD_UPDATE_SURFACE:
    return {.type = D9C_COMMAND_RECORD_UPDATE_SURFACE,
            .payload = bytesOf(D9CCommandChunkWireUpdateSurface{
                .srcHandleIndex = 0u, .dstHandleIndex = 1u}),
            .handles = {handleEntry(D9C_CHUNK_HANDLE_KIND_SURFACE, 36),
                        handleEntry(D9C_CHUNK_HANDLE_KIND_SURFACE, 37)}};
  case D9C_COMMAND_RECORD_QUERY_ISSUE:
    return queryIssueRecord(0u);
  case D9C_COMMAND_RECORD_READBACK:
    return {.type = D9C_COMMAND_RECORD_READBACK,
            .payload = bytesOf(D9CCommandChunkWireReadback{
                .srcHandleIndex = 0u, .dstHandleIndex = 1u}),
            .handles = {handleEntry(D9C_CHUNK_HANDLE_KIND_SURFACE, 38),
                        handleEntry(D9C_CHUNK_HANDLE_KIND_SURFACE, 39)}};
  case D9C_COMMAND_RECORD_RESZ_DEPTH_RESOLVE:
    return {.type = D9C_COMMAND_RECORD_RESZ_DEPTH_RESOLVE,
            .payload = bytesOf(D9CCommandChunkWireReszDepthResolve{
                .msaaDepthHandleIndex = 0u, .intzDestHandleIndex = 1u}),
            .handles = {handleEntry(D9C_CHUNK_HANDLE_KIND_SURFACE, 40),
                        handleEntry(D9C_CHUNK_HANDLE_KIND_TEXTURE, 41)}};
  case D9C_COMMAND_RECORD_GENERATE_MIPMAPS:
    return {.type = D9C_COMMAND_RECORD_GENERATE_MIPMAPS,
            .payload = bytesOf(D9CCommandChunkWireGenerateMipmaps{
                .textureHandleIndex = 0u}),
            .handles = {handleEntry(D9C_CHUNK_HANDLE_KIND_TEXTURE, 42)}};
  default:
    break;
  }
  throw TestFailure("no fixture for record type " + std::to_string(type));
}

void everyLiveKindHasOneRangeClass() {
  std::size_t eligible = 0;
  std::size_t unsupported = 0;
  for (const auto& row : dxmt9::d3d9::kRecordTopology) {
    const auto spec = recordForType(row.type);
    const std::array<RecordSpec, 1> specs{spec};
    const auto fixture = makeValidatedFixture(specs);
    ImportedChunkView imported;
    try {
      imported = fixture.view();
    } catch (const TestFailure& error) {
      throw TestFailure(std::string(error.what()) + " for record type " +
                        std::to_string(row.type));
    }

    const auto rangeClass =
        dxmt9::d3d9::classifyDirectChunkSlotRange(imported);
    // A single-record range holding a draw is Eligible; a single-record
    // island-resident state record has no draw and is therefore Empty.
    const bool islandDraw =
        row.topology == RecordReplayTopology::DirectDraw;
    const auto expected =
        islandDraw ? dxmt9::d3d9::DirectChunkSlotRangeClass::Eligible
                   : (row.islandResident
                          ? dxmt9::d3d9::DirectChunkSlotRangeClass::Empty
                          : dxmt9::d3d9::DirectChunkSlotRangeClass::Unsupported);
    check(rangeClass == expected,
          "record type " + std::to_string(row.type) +
              " must take the range class its topology row declares");
    if (rangeClass == dxmt9::d3d9::DirectChunkSlotRangeClass::Eligible) {
      ++eligible;
    } else if (rangeClass ==
               dxmt9::d3d9::DirectChunkSlotRangeClass::Unsupported) {
      ++unsupported;
    }

    // The exact plan exists exactly when the range is eligible, and it always
    // reports the draw it admitted.
    const auto plan =
        dxmt9::d3d9::planDirectChunkSlotRange(imported, 4096u);
    check(plan.has_value() == islandDraw,
          "an exact direct-slot plan exists exactly for an eligible draw "
          "range (type " + std::to_string(row.type) + ")");
    if (plan) {
      check(plan->eligible() && plan->drawCount == 1u &&
                plan->capacity.commandHeaders == 1u,
            "a one-draw range plans exactly one command header");
    }

    // The emission partition must agree with the same table on the same
    // record, so the cheap gate and the source-wide partition cannot drift.
    const auto emission =
        dxmt9::d3d9::planReplayEmission(imported, 7u, 4096u);
    check(emission.partitioned() && emission.segments.size() == 1u,
          "a single valid record is always a one-segment partition");
    const auto kind = emission.segments.front().kind;
    switch (row.topology) {
    case RecordReplayTopology::DirectDraw:
      check(kind == EmissionSegmentKind::DirectIsland,
            "a non-fan draw is a direct island");
      break;
    case RecordReplayTopology::DirectStateOnly:
      check(kind == EmissionSegmentKind::StateOnlyRun,
            "a lone state record is a state-only run");
      break;
    case RecordReplayTopology::CoordinatorCommand:
      check(kind == EmissionSegmentKind::CoordinatorLocator,
            "a coordinator command is a single-record locator");
      break;
    case RecordReplayTopology::OrderedControl:
      check(kind == EmissionSegmentKind::OrderedControlLocator,
            "an ordered control is a single-record locator");
      break;
    case RecordReplayTopology::CompatibilityDraw:
      check(kind == EmissionSegmentKind::CompatibilityRange,
            "a UP draw is a compatibility range");
      break;
    case RecordReplayTopology::Count:
      throw TestFailure("topology row must carry a live class");
    }
  }
  // Two non-fan draw kinds are eligible; the seven state kinds are Empty;
  // the remaining twelve are Unsupported for this whole-range gate.
  check(eligible == 2u, "exactly two record kinds open an eligible range");
  check(unsupported == 12u,
        "exactly twelve record kinds are outside the island alphabet");
}

// A TriangleList draw is island-eligible; the identical record with
// TriangleFan is not, and the difference must be visible in both the cheap
// gate and the emission partition.
void triangleFanLeavesTheIslandAlphabet() {
  for (const auto type : {D9C_COMMAND_RECORD_DRAW_PRIMITIVE,
                          D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE}) {
    const std::array<RecordSpec, 1> specs{
        drawRecord(static_cast<std::uint32_t>(type), /*TriangleFan=*/6u)};
    const auto fixture = makeValidatedFixture(specs);
    const auto imported = fixture.view();
    check(dxmt9::d3d9::classifyDirectChunkSlotRange(imported) ==
              dxmt9::d3d9::DirectChunkSlotRangeClass::Unsupported,
          "a TriangleFan draw is not island-eligible");
    check(!dxmt9::d3d9::planDirectChunkSlotRange(imported, 4096u).has_value(),
          "a TriangleFan draw has no exact direct-slot plan");
    const auto emission =
        dxmt9::d3d9::planReplayEmission(imported, 7u, 4096u);
    check(emission.partitioned() && emission.segments.size() == 1u &&
              emission.segments.front().kind ==
                  EmissionSegmentKind::CompatibilityRange,
          "a TriangleFan draw partitions as a compatibility range");
    check(emission.leaseBlock == EmissionLeaseBlock::CompatibilityRange,
          "a TriangleFan draw blocks the whole-raw lease");
  }
}

// ---------------------------------------------------------------------------
// T1 -- kRecordTopology conservation against the other live taxonomies.
//
// The compile-time asserts in device_c_chunk_schema.hpp already pin the table
// against kRecordRules. These runtime checks pin it against the two semantic
// taxonomies that live in other translation units and therefore cannot be
// reached from a constant expression there.
void topologyTableAgreesWithLiveTaxonomies() {
  check(dxmt9::d3d9::kRecordTopology.size() ==
            dxmt9::d3d9::kRecordRules.size(),
        "topology table covers exactly the live record alphabet");
  std::size_t orderedControls = 0;
  std::size_t islandResident = 0;
  for (const auto& row : dxmt9::d3d9::kRecordTopology) {
    const auto info = dxmt9::d3d9::devicec::replayInfoForCommandRecordType(row.type);
    check(info.category != ImportedRecordReplayCategory::Unknown,
          "every topology row has a live semantic replay category");

    // DirectStateOnly is exactly the replay sink's own stateOnly predicate.
    const bool stateOnly =
        info.category == ImportedRecordReplayCategory::ConstantUpload ||
        info.category == ImportedRecordReplayCategory::StateApply;
    check(stateOnly ==
              (row.topology == RecordReplayTopology::DirectStateOnly),
          "DirectStateOnly matches the ConstantUpload/StateApply category");

    // A synchronous read boundary is always an ordered control, and every
    // ordered control is exactly a kind makeOrderedControlDisposition owns.
    if (row.topology == RecordReplayTopology::OrderedControl) {
      ++orderedControls;
      check(row.type == D9C_COMMAND_RECORD_QUERY_ISSUE ||
                row.type == D9C_COMMAND_RECORD_READBACK ||
                row.type == D9C_COMMAND_RECORD_UPDATE_TEXTURE,
            "ordered controls are exactly Query/Readback/UpdateTexture");
    }
    if (row.islandResident) ++islandResident;
  }
  check(orderedControls == 3u, "exactly three ordered-control kinds exist");
  // Two non-fan draw kinds plus six constant setters plus APPLY_STATE.
  check(islandResident == 9u,
        "island residency covers the two draw kinds and seven state kinds");
}

// ---------------------------------------------------------------------------
// T2 -- the exhaustive partition truth table.

struct CaseReport {
  std::string label;
  std::size_t leaseEligible = 0;
};

void verifyPlanForSequence(std::span<const Klass> sequence,
                           const std::string& label,
                           CaseReport& report) {
  std::vector<RecordSpec> specs;
  specs.reserve(sequence.size());
  std::uint32_t handleBase = 0;
  for (const auto klass : sequence) {
    specs.push_back(recordForClass(klass, handleBase));
    handleBase += static_cast<std::uint32_t>(specs.back().handles.size());
  }
  const auto fixture = makeValidatedFixture(specs);
  const auto imported = fixture.view();
  const auto plan = dxmt9::d3d9::planReplayEmission(imported, 7u, 4096u);

  // I1 -- every structurally sound view yields a total partition.
  check(plan.partitioned(),
        label + ": a sound view always produces a total partition");
  check(plan.reason == EmissionPlanReason::Complete,
        label + ": a sound view completes plan construction");

  // I1 -- exact coverage with no gap, overlap, duplicate, or empty segment.
  std::size_t covered = 0;
  for (const auto& segment : plan.segments) {
    check(segment.recordCount != 0, label + ": no empty segment");
    check(segment.firstRecordIndex == covered,
          label + ": segments are contiguous in source order");
    covered += segment.recordCount;
  }
  check(covered == sequence.size(),
        label + ": segments cover the whole record range exactly once");

  // Maximality -- no two adjacent segments of a mergeable kind.
  for (std::size_t i = 1; i < plan.segments.size(); ++i) {
    const auto previous = plan.segments[i - 1].kind;
    const auto current = plan.segments[i].kind;
    check(!(previous == EmissionSegmentKind::DirectIsland &&
            current == EmissionSegmentKind::DirectIsland),
          label + ": direct islands are maximal");
    check(!(previous == EmissionSegmentKind::CompatibilityRange &&
            current == EmissionSegmentKind::CompatibilityRange),
          label + ": compatibility ranges are maximal");
    check(!(previous == EmissionSegmentKind::StateOnlyRun &&
            current == EmissionSegmentKind::StateOnlyRun),
          label + ": state-only runs are maximal");
  }

  // Segment membership -- each segment's records match its kind.
  std::size_t islands = 0;
  std::size_t coordinators = 0;
  std::size_t orderedControls = 0;
  std::size_t compatibilityRecords = 0;
  std::size_t islandDraws = 0;
  for (const auto& segment : plan.segments) {
    const auto first = segment.firstRecordIndex;
    switch (segment.kind) {
    case EmissionSegmentKind::DirectIsland: {
      std::size_t draws = 0;
      for (std::uint32_t i = 0; i < segment.recordCount; ++i) {
        const auto klass = sequence[first + i];
        check(islandResidentClass(klass),
              label + ": a direct island holds only island-resident records");
        if (klass == Klass::IslandDraw) ++draws;
      }
      check(draws != 0 && draws == segment.drawCount,
            label + ": a direct island has a non-zero exact draw count");
      // Trailing state belongs to the island; a leading state run does too.
      ++islands;
      islandDraws += draws;
      break;
    }
    case EmissionSegmentKind::StateOnlyRun:
      for (std::uint32_t i = 0; i < segment.recordCount; ++i) {
        check(sequence[first + i] == Klass::StateOnly,
              label + ": a state-only run holds only state records");
      }
      check(segment.drawCount == 0,
            label + ": a state-only run carries no draw");
      check(segment.capacity.commandHeaders == 0,
            label + ": a state-only run reserves no command header");
      break;
    case EmissionSegmentKind::CoordinatorLocator:
      check(segment.recordCount == 1u,
            label + ": a coordinator locator is exactly one record");
      check(sequence[first] == Klass::Coordinator,
            label + ": a coordinator locator holds a coordinator record");
      check(segment.locatorRecordType == D9C_COMMAND_RECORD_CLEAR,
            label + ": a coordinator locator names its wire record type");
      check(segment.capacity.commandHeaders == 1u,
            label + ": a coordinator locator reserves one command header");
      ++coordinators;
      break;
    case EmissionSegmentKind::OrderedControlLocator:
      check(segment.recordCount == 1u,
            label + ": an ordered-control locator is exactly one record");
      check(sequence[first] == Klass::OrderedControl,
            label + ": an ordered-control locator holds a control record");
      check(segment.capacity.commandHeaders == 0,
            label + ": an ordered control reserves no final-slot capacity");
      ++orderedControls;
      break;
    case EmissionSegmentKind::CompatibilityRange:
      for (std::uint32_t i = 0; i < segment.recordCount; ++i) {
        const auto klass = sequence[first + i];
        check(klass == Klass::Compatibility || klass == Klass::StateOnly,
              label + ": a compatibility range holds only compatibility "
                      "draws and the state leading into them");
      }
      check(sequence[first + segment.recordCount - 1u] ==
                Klass::Compatibility,
            label + ": a compatibility range ends on a compatibility draw");
      check(segment.capacity.commandHeaders == 0,
            label + ": a compatibility range reserves no final-slot capacity");
      compatibilityRecords += segment.recordCount;
      break;
    case EmissionSegmentKind::Count:
      check(false, label + ": no segment may carry the sentinel kind");
      break;
    }
  }
  check(islands == plan.islandCount, label + ": island count is exact");
  check(coordinators == plan.coordinatorCount,
        label + ": coordinator count is exact");
  check(orderedControls == plan.orderedControlCount,
        label + ": ordered-control count is exact");
  check(compatibilityRecords == plan.compatibilityRecordCount,
        label + ": compatibility record count is exact");
  check(islandDraws == plan.directDrawCount,
        label + ": direct draw count is the sum of island draws");

  // Lease-block precedence is a total function of the record multiset.
  const bool anyOrderedControl = orderedControls != 0;
  const bool anyCompatibility = compatibilityRecords != 0;
  const auto expectedBlock =
      anyOrderedControl ? EmissionLeaseBlock::OrderedControl
      : anyCompatibility ? EmissionLeaseBlock::CompatibilityRange
      : islands == 0 ? EmissionLeaseBlock::NoIsland
                     : EmissionLeaseBlock::None;
  check(plan.leaseBlock == expectedBlock,
        label + ": lease block follows the documented precedence");
  check(plan.leaseEligible() == (expectedBlock == EmissionLeaseBlock::None),
        label + ": lease eligibility agrees with the lease block");

  if (!plan.leaseEligible()) {
    check(plan.aggregatePlannedBytes == 0 &&
              plan.aggregateCapacity.commandHeaders == 0 &&
              plan.slotCommandCount == 0,
          label + ": a lease-blocked raw publishes no reservation");
    return;
  }
  ++report.leaseEligible;

  // I6 -- the aggregate reserves once, and covers every island exactly.
  check(plan.slotCommandCount == islandDraws + coordinators,
        label + ": slot commands are island draws plus coordinator locators");
  check(plan.aggregateCapacity.commandHeaders == plan.slotCommandCount,
        label + ": the aggregate header count equals the slot command count");
  check(plan.aggregateCapacity.drawParams == islandDraws,
        label + ": the aggregate carries one draw param per island draw");
  check(plan.aggregateCapacity.clearRecords == coordinators,
        label + ": the aggregate carries one clear record per locator");
  check(plan.aggregatePlannedBytes != 0,
        label + ": a lease-eligible raw has an exact planned byte count");

  // The non-additive derived dimensions are computed once from the total draw
  // count. Summing per-island bucket counts would under-reserve, so assert the
  // aggregate against a fresh single computation, and assert that no island's
  // own derived figure exceeds it.
  dxmt9::core::SourcePayloadCapacity derived{};
  derived.drawUniformPayloadLookupNext = islandDraws;
  const auto buckets =
      dxmt9::core::detail::chunkSlotUniformLookupBucketCount(islandDraws);
  check(plan.aggregateCapacity.drawUniformPayloadLookupHeads == buckets &&
            plan.aggregateCapacity.drawUniformPayloadLookupTails == buckets &&
            plan.aggregateCapacity.drawUniformPayloadLookupNext == islandDraws,
        label + ": uniform lookup buckets are derived once from all draws");
  std::size_t summedIslandHeads = 0;
  std::size_t summedIslandDrawParams = 0;
  for (const auto& segment : plan.segments) {
    if (segment.kind != EmissionSegmentKind::DirectIsland) continue;
    summedIslandHeads += segment.capacity.drawUniformPayloadLookupHeads;
    summedIslandDrawParams += segment.capacity.drawParams;
    check(segment.capacity.drawUniformPayloadLookupHeads <=
              plan.aggregateCapacity.drawUniformPayloadLookupHeads,
          label + ": no island claims more lookup buckets than the aggregate");
  }
  check(summedIslandDrawParams == plan.aggregateCapacity.drawParams,
        label + ": island draw params sum to the aggregate exactly");
  if (islands > 1) {
    check(summedIslandHeads >= buckets,
          label + ": per-island bucket sums are not the aggregate figure");
  }

  // The disposition is a total function and never reports a sound view as
  // structurally invalid.
  const auto disposition =
      dxmt9::d3d9::emissionPlanDisposition(plan, /*captureOrTrace=*/false);
  check(disposition !=
            dxmt9::d3d9::DirectChunkSlotReplayDisposition::RejectInvalid,
        label + ": a sound plan is never reported as invalid");
  check(dxmt9::d3d9::emissionPlanDisposition(plan, /*captureOrTrace=*/true) ==
            dxmt9::d3d9::DirectChunkSlotReplayDisposition::LegacyCaptureOrTrace,
        label + ": capture/trace always owns its own typed disposition");
}

void exhaustivePartitionTruthTable() {
  CaseReport report;
  std::vector<Klass> sequence;
  std::size_t cases = 0;
  for (std::size_t length = 1; length <= 5; ++length) {
    sequence.assign(length, Klass::IslandDraw);
    std::size_t total = 1;
    for (std::size_t i = 0; i < length; ++i) total *= kClassCount;
    for (std::size_t code = 0; code < total; ++code) {
      std::size_t rest = code;
      std::string label;
      label.reserve(length);
      for (std::size_t i = 0; i < length; ++i) {
        sequence[i] = static_cast<Klass>(rest % kClassCount);
        rest /= kClassCount;
        label.push_back(klassLetter(sequence[i]));
      }
      verifyPlanForSequence(sequence, label, report);
      ++cases;
    }
  }
  check(cases == 3905u,
        "the enumeration covers every class sequence of length 1..5");
  check(report.leaseEligible != 0,
        "the enumeration reaches lease-eligible shapes");
}

// ---------------------------------------------------------------------------
// T3 -- named regressions for the shapes the architecture exists to change.

// ---------------------------------------------------------------------------
// Present is not appendable at its serial index.
//
// `appendActiveDirectChunkSlotPresent` parks the Present in the queue-owned
// build context; `commitDirectChunkSlotReplay` appends it once, last, as the
// slot's publication boundary. Two consequences the classifier must express:
//
//  * anything a lease appends AFTER a Present would execute BEFORE it, so a
//    non-trailing Present is a source-order violation, not a shape the lease
//    can merely represent badly;
//  * a second Present in one transaction is refused outright
//    (`if (context.presentAppended) return false;`), which poisons the build.
//
// Both must therefore be a pre-effect whole-raw fallback. Earlier coordinators
// (a leading Clear) carry no such constraint and must NOT disqualify the
// Present tail -- only Present locators are counted.
void presentOrderingIsClassifiedBeforeAnyEffect() {
  // Handle indices in a record payload are chunk-global, so a Present's own
  // source index depends on how many handles the records before it added.
  const auto presentRecord = [](std::uint32_t handleBase = 0u) {
    D9CCommandChunkWirePresent present{};
    present.sourceHandleIndex = handleBase;
    return RecordSpec{.type = D9C_COMMAND_RECORD_PRESENT,
                      .payload = bytesOf(present),
                      .handles = {D9CCommandChunkWireHandleEntry{
                          .kind = D9C_CHUNK_HANDLE_KIND_SURFACE,
                          .generation = 1,
                          .objectId = 31,
                      }}};
  };

  // 1. Draw -> Present -> Draw. The second draw would be appended into the
  //    same lease and would execute before the parked Present.
  {
    const std::array records{islandDrawRecord(), presentRecord(),
                             islandDrawRecord()};
    const auto fixture = makeValidatedFixture(records);
    const auto plan =
        dxmt9::d3d9::planReplayEmission(fixture.view(), 40u, 4096u);
    check(plan.partitioned() && plan.presentCount == 1u,
          "Draw,Present,Draw still produces a total partition");
    check(plan.leaseBlock == EmissionLeaseBlock::PresentOrdering &&
              !plan.leaseEligible() && !plan.spansExecutable(),
          "a non-trailing Present blocks the lease with PresentOrdering");
    check(dxmt9::d3d9::emissionPlanDisposition(plan, false) ==
              dxmt9::d3d9::DirectChunkSlotReplayDisposition::LegacyPresent,
          "a non-trailing Present reports LegacyPresent");
    check(plan.leaseSpans.size() == 1u && plan.leaseSpans.front().ownsLease &&
              plan.leaseSpans.front().presentCount == 1u &&
              !plan.leaseSpans.front().presentTrailingCoordinator,
          "the offending span is the one whose Present is not trailing");
  }

  // 2. Duplicate Present in one span. The second append is refused by the
  //    build context, so the transaction would poison rather than mis-order.
  {
    const std::array records{islandDrawRecord(), presentRecord(0u),
                             islandDrawRecord(), presentRecord(1u)};
    const auto fixture = makeValidatedFixture(records);
    const auto plan =
        dxmt9::d3d9::planReplayEmission(fixture.view(), 41u, 4096u);
    check(plan.partitioned() && plan.presentCount == 2u &&
              plan.coordinatorCount == 2u,
          "a duplicate Present still produces a total partition");
    check(plan.leaseBlock == EmissionLeaseBlock::PresentOrdering &&
              !plan.spansExecutable(),
          "a duplicate Present blocks the lease with PresentOrdering");
    check(dxmt9::d3d9::emissionPlanDisposition(plan, false) ==
              dxmt9::d3d9::DirectChunkSlotReplayDisposition::LegacyPresent,
          "a duplicate Present reports LegacyPresent");
    check(plan.leaseSpans.size() == 1u &&
              plan.leaseSpans.front().presentCount == 2u &&
              !plan.leaseSpans.front().presentTrailingCoordinator,
          "two Presents in one span are never a trailing coordinator");
  }

  // 3. Draw -> Present -> State. State appends nothing to the slot, but it is
  //    the span's final segment, so the Present is no longer trailing. Fail
  //    closed rather than reason about which record kinds are slot-inert.
  {
    const std::array records{islandDrawRecord(), presentRecord(),
                             applyStateRecord()};
    const auto fixture = makeValidatedFixture(records);
    const auto plan =
        dxmt9::d3d9::planReplayEmission(fixture.view(), 42u, 4096u);
    check(plan.leaseBlock == EmissionLeaseBlock::PresentOrdering &&
              dxmt9::d3d9::emissionPlanDisposition(plan, false) ==
                  dxmt9::d3d9::DirectChunkSlotReplayDisposition::LegacyPresent,
          "trailing state after a Present fails closed conservatively");
  }

  // 4. Clear -> Draw -> Draw -> Present is the contract's positive case: one
  //    Present, terminal, with an earlier coordinator that carries no ordering
  //    constraint of its own. `coordinatorCount == 1` would wrongly reject it.
  {
    const std::array records{clearRecord(), islandDrawRecord(),
                             islandDrawRecord(), presentRecord()};
    const auto fixture = makeValidatedFixture(records);
    const auto plan =
        dxmt9::d3d9::planReplayEmission(fixture.view(), 43u, 4096u);
    check(plan.leaseEligible() && plan.spansExecutable() &&
              plan.coordinatorCount == 2u && plan.presentCount == 1u,
          "a leading Clear plus a terminal Present stays lease-eligible");
    check(dxmt9::d3d9::emissionPlanDisposition(plan, false) ==
              dxmt9::d3d9::DirectChunkSlotReplayDisposition::
                  DirectWithPresentTail,
          "an earlier coordinator does not disqualify the Present tail");
    check(plan.leaseSpans.size() == 1u &&
              plan.leaseSpans.front().presentTrailingCoordinator &&
              plan.leaseSpans.front().containsTerminalPresent,
          "the span reports one trailing, terminal Present");
    check(plan.rotationFreeProductionEligible(),
          "one leading Direct lease with a Present tail is rotation-free");
  }

  // 5. Present after a compatibility cut. The cut ends the first span, so the
  //    Present is the trailing coordinator of the *second* span. Ordering is
  //    intact: the compatibility range publishes through the ordinary sink at
  //    its exact serial position, before the second span's slot.
  {
    const std::array records{islandDrawRecord(), upDrawRecord(),
                             islandDrawRecord(), presentRecord()};
    const auto fixture = makeValidatedFixture(records);
    const auto plan =
        dxmt9::d3d9::planReplayEmission(fixture.view(), 44u, 4096u);
    check(plan.partitioned() && plan.spansExecutable() &&
              plan.leaseBlock == EmissionLeaseBlock::CompatibilityRange,
          "a Present trailing its own span survives a compatibility cut");
    check(plan.leaseSpans.size() == 3u &&
              plan.leaseSpans[2].ownsLease &&
              plan.leaseSpans[2].presentCount == 1u &&
              plan.leaseSpans[2].presentTrailingCoordinator &&
              plan.leaseSpans[0].presentCount == 0u,
          "only the suffix span owns the Present, and owns it trailing");
    check(dxmt9::d3d9::emissionPlanDisposition(plan, false) ==
              dxmt9::d3d9::DirectChunkSlotReplayDisposition::LegacyUpDraw,
          "the compatibility cut keeps its own typed disposition");
  }

  // 6. A cut Present that is NOT trailing in its own span must still block the
  //    whole raw, even though the raw's lease block would otherwise be the
  //    compatibility range. Present ordering has the higher precedence
  //    precisely so a mis-ordered span cannot hide behind another reason.
  {
    const std::array records{islandDrawRecord(), upDrawRecord(),
                             islandDrawRecord(), presentRecord(),
                             islandDrawRecord()};
    const auto fixture = makeValidatedFixture(records);
    const auto plan =
        dxmt9::d3d9::planReplayEmission(fixture.view(), 45u, 4096u);
    check(plan.partitioned() && !plan.spansExecutable() &&
              plan.leaseBlock == EmissionLeaseBlock::PresentOrdering,
          "PresentOrdering outranks the compatibility-range block");
    check(dxmt9::d3d9::emissionPlanDisposition(plan, false) ==
              dxmt9::d3d9::DirectChunkSlotReplayDisposition::LegacyPresent,
          "a mis-ordered Present reports LegacyPresent, not LegacyUpDraw");
  }
}

void namedIslandShapes() {
  // A Clear between two draw runs is the shape the whole-raw gate rejects
  // outright today. The partition keeps both draw runs owned by islands and
  // the Clear at its exact serial index.
  const std::array records{
      islandDrawRecord(), islandDrawRecord(), clearRecord(),
      applyStateRecord(), islandDrawRecord(),
  };
  const auto fixture = makeValidatedFixture(records);
  const auto plan = dxmt9::d3d9::planReplayEmission(fixture.view(), 3u, 4096u);
  check(plan.leaseEligible(), "Draw,Draw,Clear,State,Draw is lease-eligible");
  check(plan.segments.size() == 3u && plan.islandCount == 2u &&
            plan.coordinatorCount == 1u,
        "a Clear separates the raw into two islands and one locator");
  check(plan.segments[0].kind == EmissionSegmentKind::DirectIsland &&
            plan.segments[0].recordCount == 2u &&
            plan.segments[0].drawCount == 2u,
        "the first island holds both leading draws");
  check(plan.segments[1].kind == EmissionSegmentKind::CoordinatorLocator &&
            plan.segments[1].firstRecordIndex == 2u &&
            plan.segments[1].recordCount == 1u,
        "the Clear keeps its exact serial index as a single-record locator");
  check(plan.segments[2].kind == EmissionSegmentKind::DirectIsland &&
            plan.segments[2].firstRecordIndex == 3u &&
            plan.segments[2].recordCount == 2u &&
            plan.segments[2].drawCount == 1u,
        "interstitial state belongs to the following island");
  check(plan.directDrawCount == 3u && plan.slotCommandCount == 4u,
        "three draws plus one coordinator make four slot commands");

  // An ordered control anywhere blocks the lease but must not stop the
  // partition from describing the rest of the raw.
  const std::array withControl{
      islandDrawRecord(), queryIssueRecord(0u), islandDrawRecord(),
  };
  const auto controlFixture = makeValidatedFixture(withControl);
  const auto controlPlan =
      dxmt9::d3d9::planReplayEmission(controlFixture.view(), 4u, 4096u);
  check(controlPlan.partitioned() && controlPlan.segments.size() == 3u &&
            controlPlan.islandCount == 2u &&
            controlPlan.orderedControlCount == 1u,
        "an ordered control still yields a total, described partition");
  check(controlPlan.leaseBlock == EmissionLeaseBlock::OrderedControl &&
            !controlPlan.leaseEligible(),
        "an ordered control blocks the lease with its own typed reason");
  check(dxmt9::d3d9::emissionPlanDisposition(controlPlan, false) ==
            dxmt9::d3d9::DirectChunkSlotReplayDisposition::
                InlineOrderedControl,
        "the ordered-control block maps to its own typed disposition");

  // A terminal Present is the R-BACK-2.87 shape and keeps its own
  // disposition so the population stays distinguishable.
  const std::array withPresent{
      islandDrawRecord(),
      RecordSpec{.type = D9C_COMMAND_RECORD_PRESENT,
                 .payload = bytesOf(D9CCommandChunkWirePresent{}),
                 .handles = {D9CCommandChunkWireHandleEntry{
                     .kind = D9C_CHUNK_HANDLE_KIND_SURFACE,
                     .generation = 1,
                     .objectId = 21,
                 }}},
  };
  const auto presentFixture = makeValidatedFixture(withPresent);
  const auto presentPlan =
      dxmt9::d3d9::planReplayEmission(presentFixture.view(), 5u, 4096u);
  check(presentPlan.leaseEligible() && presentPlan.containsPresent &&
            presentPlan.coordinatorCount == 1u,
        "a terminal Present is an ordinary coordinator locator");
  check(dxmt9::d3d9::emissionPlanDisposition(presentPlan, false) ==
            dxmt9::d3d9::DirectChunkSlotReplayDisposition::
                DirectWithPresentTail,
        "an island plus a terminal Present reports DirectWithPresentTail");
  check(presentPlan.presentCount == 1u &&
            presentPlan.leaseSpans.size() == 1u &&
            presentPlan.leaseSpans.front().presentCount == 1u &&
            presentPlan.leaseSpans.front().presentTrailingCoordinator,
        "the Present-tail span reports exactly one trailing Present");

  // A state-only raw has nothing to construct; the partition still covers it.
  const std::array stateOnly{applyStateRecord(), applyStateRecord()};
  const auto stateFixture = makeValidatedFixture(stateOnly);
  const auto statePlan =
      dxmt9::d3d9::planReplayEmission(stateFixture.view(), 6u, 4096u);
  check(statePlan.partitioned() && statePlan.segments.size() == 1u &&
            statePlan.segments[0].kind == EmissionSegmentKind::StateOnlyRun &&
            statePlan.segments[0].recordCount == 2u,
        "a state-only raw is one maximal state-only run");
  check(statePlan.leaseBlock == EmissionLeaseBlock::NoIsland,
        "a state-only raw blocks the lease with NoIsland");

  // Leading state ahead of a UP draw belongs to the compatibility range, and
  // the range stays maximal across an interstitial state record.
  const std::array compatibility{
      applyStateRecord(), upDrawRecord(), applyStateRecord(), upDrawRecord(),
  };
  const auto compatibilityFixture = makeValidatedFixture(compatibility);
  const auto compatibilityPlan =
      dxmt9::d3d9::planReplayEmission(compatibilityFixture.view(), 8u, 4096u);
  check(compatibilityPlan.partitioned() &&
            compatibilityPlan.segments.size() == 1u &&
            compatibilityPlan.segments[0].kind ==
                EmissionSegmentKind::CompatibilityRange &&
            compatibilityPlan.segments[0].recordCount == 4u,
        "state leading into UP draws joins one maximal compatibility range");
  check(compatibilityPlan.leaseBlock == EmissionLeaseBlock::CompatibilityRange,
        "a compatibility record blocks the lease with its own reason");
}

// ---------------------------------------------------------------------------
// Executable lease spans. The descriptive partition says what the raw is; the
// span list says how it executes. Ordered controls and compatibility ranges
// are the only cuts, and a coordinator locator stays inside the span that
// owns it.

void leaseSpansPartitionTheRawExecutably() {
  // Draw, Draw, QueryIssue, Draw -- the smallest shape with two direct spans
  // separated by an executed ordered control, which is exactly the raw
  // `specs/verification/tla/ReplayEmissionPlanIslands.tla` models.
  const std::array specs{islandDrawRecord(), islandDrawRecord(),
                         queryIssueRecord(0u), islandDrawRecord()};
  const auto fixture = makeValidatedFixture(specs);
  const auto imported = fixture.view();
  const auto plan = dxmt9::d3d9::planReplayEmission(imported, 11u, 4096u);
  check(plan.partitioned(), "the mixed raw still partitions");
  check(plan.leaseBlock == EmissionLeaseBlock::OrderedControl,
        "an ordered control still blocks a single whole-raw lease");
  check(plan.leaseSpanCount == 3u, "the raw executes as three spans");

  const auto& first = plan.leaseSpans[0];
  const auto& separator = plan.leaseSpans[1];
  const auto& last = plan.leaseSpans[2];
  check(first.ownsLease && first.firstRecordIndex == 0u &&
            first.recordCount == 2u && first.drawCount == 2u &&
            first.islandCount == 1u && first.coordinatorCount == 0u &&
            first.leaseOrdinal == 0u && !first.finalLeaseSpan &&
            first.trailingCut == dxmt9::d3d9::EmissionSpanCut::OrderedControl,
        "the prefix island owns lease ordinal 0 and is cut by the control");
  check(!separator.ownsLease && separator.firstRecordIndex == 2u &&
            separator.recordCount == 1u &&
            separator.trailingCut ==
                dxmt9::d3d9::EmissionSpanCut::OrderedControl,
        "the ordered control is its own ordinary single-record span");
  check(last.ownsLease && last.firstRecordIndex == 3u &&
            last.recordCount == 1u && last.drawCount == 1u &&
            last.leaseOrdinal == 1u && last.finalLeaseSpan &&
            last.trailingCut == dxmt9::d3d9::EmissionSpanCut::EndOfRaw,
        "the suffix island owns the adjacent lease ordinal and settles");

  // Lease ordinals are dense over lease-owning spans only, so the interleaved
  // ordinary span does not create a gap the queue witness would read as a
  // skipped span.
  check(last.leaseOrdinal == first.leaseOrdinal + 1u,
        "an interleaved ordinary span does not consume a lease ordinal");
  check(plan.leaseOwningSpanCount() == 2u, "exactly two spans own a lease");

  // Exact coverage of the executable partition.
  std::size_t covered = 0;
  std::size_t coveredSegments = 0;
  for (const auto& span : plan.leaseSpans) {
    check(span.firstRecordIndex == covered, "spans are gap-free and ordered");
    covered += span.recordCount;
    coveredSegments += span.segmentCount;
  }
  check(covered == imported.records.size() &&
            coveredSegments == plan.segments.size(),
        "spans cover every record and every segment exactly once");

  // Each lease-owning span reserves only its own draws, and its command
  // header total is its draws plus its coordinator locators.
  for (const auto& span : plan.leaseSpans) {
    if (!span.ownsLease) {
      check(span.plannedBytes == 0u && span.capacity.commandHeaders == 0u,
            "an ordinary span reserves no final-slot capacity");
      continue;
    }
    check(span.plannedBytes != 0u &&
              span.capacity.commandHeaders ==
                  span.drawCount + span.coordinatorCount,
          "a lease span reserves one header per draw and per locator");
    check(span.capacity.drawParams == span.drawCount,
          "a lease span reserves exactly its own draw params");
  }
}

void coordinatorLocatorsStayInsideTheirSpan() {
  // Draw, Clear, Draw is one span: the Clear terminates the first island but
  // is not a cut, so both islands and the locator share one lease.
  const std::array specs{islandDrawRecord(), clearRecord(),
                         islandDrawRecord()};
  const auto fixture = makeValidatedFixture(specs);
  const auto imported = fixture.view();
  const auto plan = dxmt9::d3d9::planReplayEmission(imported, 12u, 4096u);
  check(plan.partitioned() && plan.leaseBlock == EmissionLeaseBlock::None,
        "a coordinator alone does not block the lease");
  check(plan.leaseSpanCount == 1u, "a coordinator does not cut the raw");
  const auto& span = plan.leaseSpans.front();
  check(span.ownsLease && span.recordCount == 3u && span.drawCount == 2u &&
            span.islandCount == 2u && span.coordinatorCount == 1u &&
            span.leaseOrdinal == 0u && span.finalLeaseSpan,
        "one lease span owns both islands and the locator between them");
  check(span.capacity.commandHeaders == 3u && span.capacity.clearRecords == 1u,
        "the span reserves two draw headers plus the Clear locator");

  // The non-additive derived dimensions are computed once from the span's
  // total draw count. Summing the two islands' per-segment capacities would
  // under-reserve the uniform lookup buckets, and an append that had to grow
  // final storage inside the transaction is what R-BACK-2.86 forbids.
  const auto perIslandBuckets =
      dxmt9::core::detail::chunkSlotUniformLookupBucketCount(1u) * 2u;
  const auto spanBuckets =
      dxmt9::core::detail::chunkSlotUniformLookupBucketCount(2u);
  check(span.capacity.drawUniformPayloadLookupHeads == spanBuckets,
        "lookup buckets are derived once from the span draw count");
  check(spanBuckets != perIslandBuckets ||
            span.capacity.drawUniformPayloadLookupNext == 2u,
        "the derived dimensions describe the whole span, not one island");
}

void compatibilityRangeDoesNotPoisonNeighbouringSpans() {
  // Draw, UP-draw, Draw. The compatibility range is a cut, so it ends the
  // span before it and delays the span after it -- but neither island loses
  // its lease.
  const std::array specs{islandDrawRecord(), upDrawRecord(),
                         islandDrawRecord()};
  const auto fixture = makeValidatedFixture(specs);
  const auto imported = fixture.view();
  const auto plan = dxmt9::d3d9::planReplayEmission(imported, 13u, 4096u);
  check(plan.partitioned() &&
            plan.leaseBlock == EmissionLeaseBlock::CompatibilityRange,
        "a UP draw still blocks a single whole-raw lease");
  check(plan.leaseSpanCount == 3u && plan.leaseOwningSpanCount() == 2u,
        "the islands either side of a compatibility range keep their leases");
  check(plan.leaseSpans[0].ownsLease && plan.leaseSpans[0].leaseOrdinal == 0u &&
            !plan.leaseSpans[1].ownsLease &&
            plan.leaseSpans[1].trailingCut ==
                dxmt9::d3d9::EmissionSpanCut::CompatibilityRange &&
            plan.leaseSpans[2].ownsLease &&
            plan.leaseSpans[2].leaseOrdinal == 1u &&
            plan.leaseSpans[2].finalLeaseSpan,
        "the compatibility range is an ordinary span between two direct ones");
  check(!plan.rotationFreeProductionEligible(),
        "multiple Direct leases fail closed before production effects");

  const std::array prefixSpecs{upDrawRecord(), islandDrawRecord()};
  const auto prefixFixture = makeValidatedFixture(prefixSpecs);
  const auto prefixPlan = dxmt9::d3d9::planReplayEmission(
      prefixFixture.view(), 15u, 4096u);
  check(prefixPlan.partitioned() && prefixPlan.leaseOwningSpanCount() == 1u &&
            !prefixPlan.leaseSpans.front().ownsLease &&
            !prefixPlan.rotationFreeProductionEligible(),
        "an ordinary prefix before the only Direct lease fails closed");
}

void aRawWithNoIslandOwnsNoLease() {
  const std::array specs{applyStateRecord(), clearRecord()};
  const auto fixture = makeValidatedFixture(specs);
  const auto imported = fixture.view();
  const auto plan = dxmt9::d3d9::planReplayEmission(imported, 14u, 4096u);
  check(plan.partitioned() && plan.leaseBlock == EmissionLeaseBlock::NoIsland,
        "a draw-free raw has no island to construct");
  check(plan.leaseOwningSpanCount() == 0u,
        "a draw-free raw owns no lease and keeps its ordinary ownership");
  for (const auto& span : plan.leaseSpans) {
    check(!span.ownsLease && span.plannedBytes == 0u,
          "no span of a draw-free raw reserves final-slot capacity");
  }
}

void malformedViewsFailClosed() {
  const std::array records{islandDrawRecord()};
  const auto fixture = makeValidatedFixture(records);
  const auto imported = fixture.view();
  check(dxmt9::d3d9::planReplayEmission(imported, 0u, 4096u).reason ==
            EmissionPlanReason::MalformedView,
        "a zero raw ordinal is a malformed-plan rejection");
  check(dxmt9::d3d9::planReplayEmission(imported, 9u, 0u).reason ==
            EmissionPlanReason::MalformedView,
        "a zero page size is a malformed-plan rejection");
  auto mismatched = imported;
  mismatched.header.recordCount += 1u;
  const auto plan = dxmt9::d3d9::planReplayEmission(mismatched, 9u, 4096u);
  check(plan.reason == EmissionPlanReason::MalformedView &&
            !plan.partitioned() &&
            plan.leaseBlock == EmissionLeaseBlock::IncompletePlan,
        "a view disagreeing with its own record table fails closed");
  check(dxmt9::d3d9::emissionPlanDisposition(plan, false) ==
            dxmt9::d3d9::DirectChunkSlotReplayDisposition::RejectInvalid,
        "a malformed view is a hard reject, not a legacy fallback");
}

}  // namespace

int main() {
  try {
    topologyTableAgreesWithLiveTaxonomies();
    everyLiveKindHasOneRangeClass();
    triangleFanLeavesTheIslandAlphabet();
    exhaustivePartitionTruthTable();
    namedIslandShapes();
    presentOrderingIsClassifiedBeforeAnyEffect();
    leaseSpansPartitionTheRawExecutably();
    coordinatorLocatorsStayInsideTheirSpan();
    compatibilityRangeDoesNotPoisonNeighbouringSpans();
    aRawWithNoIslandOwnsNoLease();
    malformedViewsFailClosed();
  } catch (const std::exception& error) {
    std::cerr << "replay_emission_plan_spec failed: " << error.what() << '\n';
    return 1;
  }
  return 0;
}
