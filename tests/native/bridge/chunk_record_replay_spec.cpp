#include "chunk_record_import_spec_fixtures.hpp"

namespace {

using namespace dxmt9::d3d9::devicec::spec;

void testImportedWireDrawRunScansRecordTableOrder() {
  const auto firstDraw = makeDrawPrimitiveRecord(3u, 1u);
  const auto secondDraw = makeDrawPrimitiveRecord(9u, 2u);

  constexpr std::uint32_t secondPayloadOffset = 16u;
  const auto firstPayloadOffset =
      static_cast<std::uint32_t>(secondPayloadOffset + sizeof(secondDraw) + 24u);
  std::vector<std::uint8_t> arena(firstPayloadOffset + sizeof(firstDraw));
  writeObject(arena, secondPayloadOffset, secondDraw);
  writeObject(arena, firstPayloadOffset, firstDraw);

  std::vector<D9CCommandChunkWireRecordHeader> records{
      D9CCommandChunkWireRecordHeader{
          .type = static_cast<std::uint32_t>(D9C_COMMAND_RECORD_DRAW_PRIMITIVE),
          .flags = D9C_COMMAND_CHUNK_WIRE_RECORD_FLAG_NONE,
          .payloadOffset = firstPayloadOffset,
          .payloadSize = static_cast<std::uint32_t>(sizeof(firstDraw)),
          .firstHandle = 0u,
          .handleCount = 0u,
          .reserved0 = 0u,
          .reserved1 = 0u,
      },
      D9CCommandChunkWireRecordHeader{
          .type = static_cast<std::uint32_t>(D9C_COMMAND_RECORD_DRAW_PRIMITIVE),
          .flags = D9C_COMMAND_CHUNK_WIRE_RECORD_FLAG_NONE,
          .payloadOffset = secondPayloadOffset,
          .payloadSize = static_cast<std::uint32_t>(sizeof(secondDraw)),
          .firstHandle = 0u,
          .handleCount = 0u,
          .reserved0 = 0u,
          .reserved1 = 0u,
      },
  };

  const auto wire = makeImportedWireChunkView(
      records.data(), static_cast<std::uint32_t>(records.size()), arena.data(),
      static_cast<std::uint32_t>(arena.size()), nullptr, 0u);
  const auto validation = validateImportedWireChunk(wire);
  check(validation.valid(), "non-contiguous wire draw records validate");

  const auto first = nextImportedRecord(wire, 0u);
  check(first.has_value(), "wire draw-run first record exists");
  check(first->valid(), "wire draw-run first record validates");
  D9CCommandRecordDrawPrimitive decodedFirst{};
  std::memcpy(&decodedFirst, first->record, sizeof(decodedFirst));
  checkEq(decodedFirst.packet.startVertex, 3u,
          "wire draw-run first payload follows record table order");

  const auto scan = scanImportedDrawRun(wire, *first);
  check(scan.replayAsRun(), "wire draw-run scanner coalesces by record table order");
  checkEq(scan.recordCount, 2u, "wire draw-run scanner record count");
  checkEq(scan.endIndex, 2u, "wire draw-run scanner advances by index");

  const auto second = nextImportedRecord(wire, first->nextIndex());
  check(second.has_value(), "wire draw-run second record exists");
  D9CCommandRecordDrawPrimitive decodedSecond{};
  std::memcpy(&decodedSecond, second->record, sizeof(decodedSecond));
  checkEq(decodedSecond.packet.startVertex, 9u,
          "wire draw-run second payload may live before first payload");
}

void testDrawRunScanBoundary() {
  const auto firstDraw = makeDrawPrimitiveRecord(0u, 1u);
  const auto secondDraw = makeDrawPrimitiveRecord(3u, 1u);
  const auto statefulDraw = makeDrawPrimitiveRecord(6u, 1u, true);
  const auto trailingDraw = makeDrawPrimitiveRecord(9u, 1u);

  std::vector<std::uint8_t> bytes;
  appendRecord(bytes, firstDraw);
  appendRecord(bytes, secondDraw);
  appendRecord(bytes, statefulDraw);
  appendRecord(bytes, trailingDraw);

  const auto chunk = makeImportedChunkView(
      bytes.data(), static_cast<std::uint32_t>(bytes.size()), 4u);
  const auto first = nextImportedRecord(chunk, 0u, 0u);
  check(first.has_value(), "draw-run scan first record exists");
  check(first->valid(), "draw-run scan first record validates");

  const auto scan = scanImportedDrawRun(chunk, *first);
  check(scan.replayAsRun(), "draw-run scan identifies a replayable run");
  checkEq(scan.recordCount, 2u, "draw-run scan stops before state delta");
  checkEq(scan.endOffset,
          static_cast<std::uint32_t>(sizeof(firstDraw) + sizeof(secondDraw)),
          "draw-run scan end offset");
  checkEq(scan.endIndex, 2u, "draw-run scan end index");
  checkEq(static_cast<int>(scan.stop),
          static_cast<int>(ImportedDrawRunScanStop::StateDelta),
          "draw-run scan reports state-delta boundary");
  checkEq(scan.stopRecord.index, 2u, "draw-run scan stop record index");
  checkEq(scan.stopRecord.offset,
          static_cast<std::uint32_t>(sizeof(firstDraw) + sizeof(secondDraw)),
          "draw-run scan stop record offset");

  const auto afterBoundary =
      nextImportedRecord(chunk, scan.stopRecord.nextOffset(), scan.stopRecord.nextIndex());
  check(afterBoundary.has_value(), "record after draw-run boundary is still reachable");
  checkEq(afterBoundary->index, 3u, "draw-run scan does not consume trailing draw");
}

void testIndexedDrawRunScanPreservesPerRecordParams() {
  auto firstDraw = makeDrawIndexedPrimitiveRecord(2u, 3u);
  firstDraw.packet.state.primitiveType = 5u;
  firstDraw.packet.baseVertex = -4;
  firstDraw.packet.minVertex = 10u;
  firstDraw.packet.numVertices = 99u;

  auto secondDraw = makeDrawIndexedPrimitiveRecord(17u, 5u);
  secondDraw.packet.state.primitiveType = 5u;
  secondDraw.packet.baseVertex = 6;
  secondDraw.packet.minVertex = 1u;
  secondDraw.packet.numVertices = 12u;

  auto statefulBoundary = makeDrawIndexedPrimitiveRecord(21u, 1u, true);
  statefulBoundary.packet.baseVertex = -1;

  std::vector<std::uint8_t> bytes;
  appendRecord(bytes, firstDraw);
  appendRecord(bytes, secondDraw);
  appendRecord(bytes, statefulBoundary);

  const auto chunk = makeImportedChunkView(
      bytes.data(), static_cast<std::uint32_t>(bytes.size()), 3u);
  const auto first = nextImportedRecord(chunk, 0u, 0u);
  check(first.has_value(), "indexed draw-run scan first record exists");
  check(first->valid(), "indexed draw-run scan first record validates");

  const auto scan = scanImportedDrawRun(chunk, *first);
  check(scan.replayAsRun(), "indexed draw-run scan identifies a replayable run");
  checkEq(scan.recordType,
          static_cast<std::uint32_t>(D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE),
          "indexed draw-run scan records indexed type");
  checkEq(scan.recordCount, 2u,
          "indexed draw-run scan stops before stateful indexed draw");
  checkEq(scan.endIndex, 2u, "indexed draw-run scan end index");
  checkEq(static_cast<int>(scan.stop),
          static_cast<int>(ImportedDrawRunScanStop::StateDelta),
          "indexed draw-run scan reports state-delta boundary");

  const auto firstParam = makeRunParam(firstDraw.packet);
  const auto secondParam = makeRunParam(secondDraw.packet);
  check(firstParam.indexed, "first indexed scan param remains indexed");
  check(secondParam.indexed, "second indexed scan param remains indexed");
  check(firstParam.primitiveType == dxmt9::core::PrimitiveType::TriangleStrip,
        "first indexed scan param maps primitive type");
  checkEq(firstParam.primitiveCount, 3u,
          "first indexed scan param keeps primitive count");
  checkEq(firstParam.baseVertexIndex, -4,
          "first indexed scan param keeps negative base vertex");
  checkEq(firstParam.startIndex, 2u,
          "first indexed scan param keeps start index");
  checkEq(secondParam.primitiveCount, 5u,
          "second indexed scan param keeps primitive count");
  checkEq(secondParam.baseVertexIndex, 6,
          "second indexed scan param keeps positive base vertex");
  checkEq(secondParam.startIndex, 17u,
          "second indexed scan param keeps independent start index");
  checkEq(firstParam.startVertex, 0u,
          "indexed scan param leaves start vertex out of direct/expanded policy data");

  const auto boundary =
      nextImportedRecord(chunk, scan.stopRecord.offset, scan.stopRecord.index);
  check(boundary.has_value(), "indexed stateful boundary record remains reachable");
  checkEq(boundary->index, 2u,
          "indexed draw-run scan does not consume stateful boundary");
}

void testImportedRecordReplayInfoClassifiesOrderingBoundaries() {
  const auto draw = replayInfoForCommandRecordType(D9C_COMMAND_RECORD_DRAW_PRIMITIVE);
  checkEq(static_cast<int>(draw.category), static_cast<int>(ImportedRecordReplayCategory::Draw),
          "draw record category");
  check(draw.ordered, "draw record is ordered");
  check(draw.mutatesDeviceState, "draw record applies packet state");
  check(draw.readsDeviceState, "draw record reads effective state");
  check(draw.referencesResources, "draw record may reference resources");
  check(draw.draw, "draw record is a draw");
  check(!draw.barrier, "draw record is not a barrier");

  const auto apply = replayInfoForCommandRecordType(D9C_COMMAND_RECORD_APPLY_STATE);
  checkEq(static_cast<int>(apply.category), static_cast<int>(ImportedRecordReplayCategory::StateApply),
          "apply-state category");
  check(apply.mutatesDeviceState, "apply-state mutates server shadow");
  check(apply.referencesResources, "apply-state may bind resources");
  check(!apply.draw, "apply-state is not a draw");

  const auto present = replayInfoForCommandRecordType(D9C_COMMAND_RECORD_PRESENT);
  checkEq(static_cast<int>(present.category), static_cast<int>(ImportedRecordReplayCategory::Present),
          "present category");
  check(present.ordered, "present is ordered");
  check(present.readsDeviceState, "present observes current render state boundary");
  check(present.barrier, "present is a replay barrier");
  check(!present.synchronousReadBoundary, "present is not a readback boundary");

  const auto readback = replayInfoForCommandRecordType(D9C_COMMAND_RECORD_READBACK);
  checkEq(static_cast<int>(readback.category), static_cast<int>(ImportedRecordReplayCategory::Readback),
          "readback category");
  check(readback.referencesResources, "readback references surfaces");
  check(readback.barrier, "readback is an ordering barrier");
  check(readback.synchronousReadBoundary, "readback is a synchronous read boundary");

  const auto unknown = replayInfoForCommandRecordType(0xffffu);
  checkEq(static_cast<int>(unknown.category), static_cast<int>(ImportedRecordReplayCategory::Unknown),
          "unknown record category");
  check(!unknown.ordered, "unknown record has no replay contract");
}

void testDrawRunHelpers() {
  D9CDrawPrimitivePacket packet{};
  packet.primitiveType = 4u;
  packet.primitiveCount = 2u;
  packet.startVertex = 7u;

  check(packetHasNoStateDelta(packet), "default draw packet has no state delta");
  auto draw = makeRunParam(packet);
  check(!draw.indexed, "draw primitive run param is not indexed");
  check(draw.primitiveType == dxmt9::core::PrimitiveType::TriangleList,
        "draw primitive maps D3D triangle list");
  checkEq(draw.primitiveCount, 2u, "draw primitive run count");
  checkEq(draw.startVertex, 7u, "draw primitive run start vertex");

  packet.renderStateCount = 1u;
  check(!packetHasNoStateDelta(packet), "render-state count marks packet dirty");

  D9CDrawIndexedPrimitivePacket indexed{};
  indexed.state.primitiveType = 5u;
  indexed.primitiveCount = 4u;
  indexed.baseVertex = -3;
  indexed.startIndex = 9u;
  auto indexedDraw = makeRunParam(indexed);
  check(indexedDraw.indexed, "indexed run param is indexed");
  check(indexedDraw.primitiveType == dxmt9::core::PrimitiveType::TriangleStrip,
        "indexed draw maps D3D triangle strip");
  checkEq(indexedDraw.primitiveCount, 4u, "indexed run count");
  checkEq(indexedDraw.baseVertexIndex, -3, "indexed run base vertex");
  checkEq(indexedDraw.startIndex, 9u, "indexed run start index");
}

}  // namespace

int main() {
  try {
    testImportedWireDrawRunScansRecordTableOrder();
    testDrawRunScanBoundary();
    testIndexedDrawRunScanPreservesPerRecordParams();
    testImportedRecordReplayInfoClassifiesOrderingBoundaries();
    testDrawRunHelpers();
  } catch (const dxmt9::d3d9::devicec::spec::TestFailure& e) {
    std::cerr << "chunk_record_replay_spec failed: " << e.what() << '\n';
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}
