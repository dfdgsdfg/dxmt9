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

void testDrawRunScanReportsConstantUploadBoundary() {
  const auto firstDraw = makeDrawPrimitiveRecord(0u, 1u);
  const auto constUpload = makeSetConstRecord(
      D9C_COMMAND_RECORD_SET_VS_CONST_F, 1u, sizeof(float) * 4u);
  const auto secondDraw = makeDrawPrimitiveRecord(3u, 1u);

  std::vector<std::uint8_t> bytes;
  appendRecord(bytes, firstDraw);
  appendRecord(bytes, constUpload, constUpload.header.size);
  appendRecord(bytes, secondDraw);

  const auto chunk = makeImportedChunkView(
      bytes.data(), static_cast<std::uint32_t>(bytes.size()), 3u);
  const auto first = nextImportedRecord(chunk, 0u, 0u);
  check(first.has_value(), "const-boundary scan first record exists");
  check(first->valid(), "const-boundary scan first record validates");

  const auto scan = scanImportedDrawRun(chunk, *first);
  check(!scan.replayAsRun(),
        "const upload is not folded into a single-uniform draw run");
  checkEq(scan.recordCount, 1u,
          "const-boundary scan keeps only the draw before the upload");
  checkEq(scan.endIndex, 1u,
          "const-boundary scan stops at the constant upload");
  checkEq(static_cast<int>(scan.stop),
          static_cast<int>(ImportedDrawRunScanStop::ConstantUpload),
          "const-boundary scan reports constant-upload stop reason");
  checkEq(scan.stopRecord.header.type,
          static_cast<std::uint32_t>(D9C_COMMAND_RECORD_SET_VS_CONST_F),
          "const-boundary scan exposes the constant upload record");
}

void testDrawRunScanUsesFirstStateDeltaAsRunBase() {
  auto firstDraw = makeDrawPrimitiveRecord(0u, 1u, true);
  const auto secondDraw = makeDrawPrimitiveRecord(3u, 1u);
  auto repeatedStateDraw = makeDrawPrimitiveRecord(6u, 1u, true);
  auto differentStateDraw = makeDrawPrimitiveRecord(9u, 1u, true);
  differentStateDraw.packet.renderStates[0].value = 12u;

  std::vector<std::uint8_t> bytes;
  appendRecord(bytes, firstDraw);
  appendRecord(bytes, secondDraw);
  appendRecord(bytes, repeatedStateDraw);
  appendRecord(bytes, differentStateDraw);

  const auto chunk = makeImportedChunkView(
      bytes.data(), static_cast<std::uint32_t>(bytes.size()), 4u);
  const auto first = nextImportedRecord(chunk, 0u, 0u);
  check(first.has_value(), "state-base draw-run scan first record exists");
  check(first->valid(), "state-base draw-run scan first record validates");

  const auto scan = scanImportedDrawRun(chunk, *first);
  check(scan.replayAsRun(), "first state delta becomes the draw-run base");
  checkEq(scan.recordCount, 3u,
          "draw-run scan accepts no-delta and repeated base-delta records");
  checkEq(scan.endIndex, 3u, "state-base draw-run scan stops before changed state");
  checkEq(static_cast<int>(scan.stop),
          static_cast<int>(ImportedDrawRunScanStop::StateDelta),
          "state-base draw-run scan reports changed-state boundary");
}

void testDrawRunScanAllowsRepeatedStreamDelta() {
  auto firstDraw = makeDrawPrimitiveRecord(0u, 1u);
  firstDraw.packet.streamSourceMask = 1u;
  firstDraw.packet.streamSources[0].buffer.lo = 0x1000u;
  firstDraw.packet.streamSources[0].offset = 16u;
  firstDraw.packet.streamSources[0].stride = 32u;
  auto repeatedStreamDraw = makeDrawPrimitiveRecord(3u, 1u);
  repeatedStreamDraw.packet.streamSourceMask = 1u;
  repeatedStreamDraw.packet.streamSources[0].buffer.lo = 0x1000u;
  repeatedStreamDraw.packet.streamSources[0].offset = 16u;
  repeatedStreamDraw.packet.streamSources[0].stride = 32u;
  auto changedStreamDraw = makeDrawPrimitiveRecord(6u, 1u);
  changedStreamDraw.packet.streamSourceMask = 1u;
  changedStreamDraw.packet.streamSources[0].buffer.lo = 0x2000u;
  changedStreamDraw.packet.streamSources[0].offset = 16u;
  changedStreamDraw.packet.streamSources[0].stride = 32u;

  std::vector<std::uint8_t> bytes;
  appendRecord(bytes, firstDraw);
  appendRecord(bytes, repeatedStreamDraw);
  appendRecord(bytes, changedStreamDraw);

  const auto chunk = makeImportedChunkView(
      bytes.data(), static_cast<std::uint32_t>(bytes.size()), 3u);
  const auto first = nextImportedRecord(chunk, 0u, 0u);
  check(first.has_value(), "stream-delta draw-run scan first record exists");
  check(first->valid(), "stream-delta draw-run scan first record validates");

  const auto scan = scanImportedDrawRun(chunk, *first);
  check(scan.replayAsRun(), "stream deltas can be carried as per-draw overrides");
  checkEq(scan.recordCount, 3u,
          "draw-run scan accepts repeated and changed stream deltas");
  checkEq(scan.endIndex, 3u,
          "draw-run scan consumes stream-only binding changes");
  checkEq(static_cast<int>(scan.stop),
          static_cast<int>(ImportedDrawRunScanStop::EndOfChunk),
          "stream-only delta scan reaches chunk end");
}

void testDrawRunScanAllowsMixedDirectAndIndexedNoDelta() {
  const auto directDraw = makeDrawPrimitiveRecord(0u, 1u);
  auto indexedDraw = makeDrawIndexedPrimitiveRecord(3u, 2u);
  indexedDraw.packet.state.primitiveType = directDraw.packet.primitiveType;
  const auto trailingDirectDraw = makeDrawPrimitiveRecord(9u, 1u);

  std::vector<std::uint8_t> bytes;
  appendRecord(bytes, directDraw);
  appendRecord(bytes, indexedDraw);
  appendRecord(bytes, trailingDirectDraw);

  const auto chunk = makeImportedChunkView(
      bytes.data(), static_cast<std::uint32_t>(bytes.size()), 3u);
  const auto first = nextImportedRecord(chunk, 0u, 0u);
  check(first.has_value(), "mixed direct/indexed scan first record exists");
  check(first->valid(), "mixed direct/indexed scan first record validates");

  const auto scan = scanImportedDrawRun(chunk, *first);
  check(scan.replayAsRun(), "mixed direct/indexed no-delta records share run state");
  checkEq(scan.recordCount, 3u,
          "draw-run scan accepts direct/indexed/direct records");
  checkEq(static_cast<int>(scan.stop),
          static_cast<int>(ImportedDrawRunScanStop::EndOfChunk),
          "mixed direct/indexed scan reaches chunk end");

  const auto directParam = makeRunParam(directDraw.packet);
  const auto indexedParam = makeRunParam(indexedDraw.packet);
  check(!directParam.indexed, "mixed run preserves direct draw params");
  check(indexedParam.indexed, "mixed run preserves indexed draw params");
}

void testDrawRunScanAllowsLateIndexBufferDeltaInMixedRun() {
  const auto directDraw = makeDrawPrimitiveRecord(0u, 1u);
  auto indexedDraw = makeDrawIndexedPrimitiveRecord(3u, 1u);
  indexedDraw.packet.state.primitiveType = directDraw.packet.primitiveType;
  indexedDraw.packet.ibValid = 1u;
  indexedDraw.packet.ibHandle.lo = 0x1000u;

  std::vector<std::uint8_t> bytes;
  appendRecord(bytes, directDraw);
  appendRecord(bytes, indexedDraw);

  const auto chunk = makeImportedChunkView(
      bytes.data(), static_cast<std::uint32_t>(bytes.size()), 2u);
  const auto first = nextImportedRecord(chunk, 0u, 0u);
  check(first.has_value(), "late-ib mixed scan first record exists");
  check(first->valid(), "late-ib mixed scan first record validates");

  const auto scan = scanImportedDrawRun(chunk, *first);
  check(scan.replayAsRun(),
        "late index-buffer delta can be carried as a per-draw override");
  checkEq(scan.recordCount, 2u,
          "late-ib mixed scan includes the indexed record");
  checkEq(static_cast<int>(scan.stop),
          static_cast<int>(ImportedDrawRunScanStop::EndOfChunk),
          "late-ib mixed scan reaches chunk end");
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

void testIndexedDrawRunScanUsesFirstIndexBufferDeltaAsRunBase() {
  auto firstDraw = makeDrawIndexedPrimitiveRecord(0u, 1u);
  firstDraw.packet.ibValid = 1u;
  firstDraw.packet.ibHandle.lo = 0x1000u;
  const auto secondDraw = makeDrawIndexedPrimitiveRecord(3u, 1u);

  std::vector<std::uint8_t> bytes;
  appendRecord(bytes, firstDraw);
  appendRecord(bytes, secondDraw);

  const auto chunk = makeImportedChunkView(
      bytes.data(), static_cast<std::uint32_t>(bytes.size()), 2u);
  const auto first = nextImportedRecord(chunk, 0u, 0u);
  check(first.has_value(), "indexed ib-delta scan first record exists");
  check(first->valid(), "indexed ib-delta scan first record validates");

  const auto scan = scanImportedDrawRun(chunk, *first);
  check(scan.replayAsRun(), "indexed first ib-delta draw becomes the run base");
  checkEq(scan.recordCount, 2u, "indexed ib-delta scan includes following no-delta draw");
  checkEq(static_cast<int>(scan.stop),
          static_cast<int>(ImportedDrawRunScanStop::EndOfChunk),
          "indexed ib-delta scan reaches chunk end");
  checkEq(scan.endIndex, 2u,
          "indexed ib-delta scan consumes the compatible run");
}

void testIndexedDrawRunScanAllowsRepeatedIndexBufferDelta() {
  auto firstDraw = makeDrawIndexedPrimitiveRecord(0u, 1u);
  firstDraw.packet.ibValid = 1u;
  firstDraw.packet.ibHandle.lo = 0x1000u;
  auto repeatedIbDraw = makeDrawIndexedPrimitiveRecord(3u, 1u);
  repeatedIbDraw.packet.ibValid = 1u;
  repeatedIbDraw.packet.ibHandle.lo = 0x1000u;
  auto changedIbDraw = makeDrawIndexedPrimitiveRecord(6u, 1u);
  changedIbDraw.packet.ibValid = 1u;
  changedIbDraw.packet.ibHandle.lo = 0x2000u;

  std::vector<std::uint8_t> bytes;
  appendRecord(bytes, firstDraw);
  appendRecord(bytes, repeatedIbDraw);
  appendRecord(bytes, changedIbDraw);

  const auto chunk = makeImportedChunkView(
      bytes.data(), static_cast<std::uint32_t>(bytes.size()), 3u);
  const auto first = nextImportedRecord(chunk, 0u, 0u);
  check(first.has_value(), "indexed repeated-ib scan first record exists");
  check(first->valid(), "indexed repeated-ib scan first record validates");

  const auto scan = scanImportedDrawRun(chunk, *first);
  check(scan.replayAsRun(), "index-buffer deltas can be carried as per-draw overrides");
  checkEq(scan.recordCount, 3u,
          "indexed draw-run scan accepts repeated and changed IB deltas");
  checkEq(scan.endIndex, 3u,
          "indexed draw-run scan consumes IB binding changes");
  checkEq(static_cast<int>(scan.stop),
          static_cast<int>(ImportedDrawRunScanStop::EndOfChunk),
          "IB-delta scan reaches chunk end");
}

void testIndexedDrawRunScanAllowsMixedDirectWithBaseIndexBuffer() {
  auto firstDraw = makeDrawIndexedPrimitiveRecord(0u, 1u);
  firstDraw.packet.ibValid = 1u;
  firstDraw.packet.ibHandle.lo = 0x1000u;
  auto directDraw = makeDrawPrimitiveRecord(3u, 1u);
  directDraw.packet.primitiveType = firstDraw.packet.state.primitiveType;
  auto repeatedIbDraw = makeDrawIndexedPrimitiveRecord(6u, 1u);
  repeatedIbDraw.packet.state.primitiveType = firstDraw.packet.state.primitiveType;
  repeatedIbDraw.packet.ibValid = 1u;
  repeatedIbDraw.packet.ibHandle.lo = 0x1000u;
  auto changedIbDraw = makeDrawIndexedPrimitiveRecord(9u, 1u);
  changedIbDraw.packet.state.primitiveType = firstDraw.packet.state.primitiveType;
  changedIbDraw.packet.ibValid = 1u;
  changedIbDraw.packet.ibHandle.lo = 0x2000u;

  std::vector<std::uint8_t> bytes;
  appendRecord(bytes, firstDraw);
  appendRecord(bytes, directDraw);
  appendRecord(bytes, repeatedIbDraw);
  appendRecord(bytes, changedIbDraw);

  const auto chunk = makeImportedChunkView(
      bytes.data(), static_cast<std::uint32_t>(bytes.size()), 4u);
  const auto first = nextImportedRecord(chunk, 0u, 0u);
  check(first.has_value(), "indexed/direct mixed scan first record exists");
  check(first->valid(), "indexed/direct mixed scan first record validates");

  const auto scan = scanImportedDrawRun(chunk, *first);
  check(scan.replayAsRun(),
        "indexed base IB can support mixed direct and per-draw IB overrides");
  checkEq(scan.recordCount, 4u,
          "indexed/direct mixed scan accepts changed IB records");
  checkEq(scan.endIndex, 4u,
          "indexed/direct mixed scan consumes changed IB");
  checkEq(static_cast<int>(scan.stop),
          static_cast<int>(ImportedDrawRunScanStop::EndOfChunk),
          "indexed/direct mixed scan reaches chunk end");
}

void testIndexedDrawRunScanAllowsDifferentIndexBufferDelta() {
  auto firstDraw = makeDrawIndexedPrimitiveRecord(0u, 1u);
  firstDraw.packet.ibValid = 1u;
  firstDraw.packet.ibHandle.lo = 0x1000u;
  auto secondDraw = makeDrawIndexedPrimitiveRecord(3u, 1u);
  secondDraw.packet.ibValid = 1u;
  secondDraw.packet.ibHandle.lo = 0x2000u;

  std::vector<std::uint8_t> bytes;
  appendRecord(bytes, firstDraw);
  appendRecord(bytes, secondDraw);

  const auto chunk = makeImportedChunkView(
      bytes.data(), static_cast<std::uint32_t>(bytes.size()), 2u);
  const auto first = nextImportedRecord(chunk, 0u, 0u);
  check(first.has_value(), "indexed different-ib scan first record exists");
  check(first->valid(), "indexed different-ib scan first record validates");

  const auto scan = scanImportedDrawRun(chunk, *first);
  check(scan.replayAsRun(), "different index buffer delta remains in the run");
  checkEq(scan.recordCount, 2u, "different ib scan includes both records");
  checkEq(static_cast<int>(scan.stop),
          static_cast<int>(ImportedDrawRunScanStop::EndOfChunk),
          "different ib scan reaches chunk end");
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

  // R-FORMAT-11: RESZ depth-resolve classifies as a fire-and-forget SurfaceOp
  // ordering barrier (the StretchRect/ColorFill class), NOT the Readback
  // synchronous-read class and NOT a draw.
  const auto resz =
      replayInfoForCommandRecordType(D9C_COMMAND_RECORD_RESZ_DEPTH_RESOLVE);
  checkEq(static_cast<int>(resz.category),
          static_cast<int>(ImportedRecordReplayCategory::SurfaceOp),
          "RESZ depth-resolve category");
  check(resz.ordered, "RESZ depth-resolve is ordered");
  check(resz.readsDeviceState, "RESZ depth-resolve observes bound depth state");
  check(resz.referencesResources, "RESZ depth-resolve references surfaces");
  check(resz.barrier, "RESZ depth-resolve is an ordering barrier");
  check(!resz.synchronousReadBoundary,
        "RESZ depth-resolve is fire-and-forget, not a synchronous read boundary");
  check(!resz.draw, "RESZ depth-resolve is not a draw");

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
    testDrawRunScanReportsConstantUploadBoundary();
    testDrawRunScanUsesFirstStateDeltaAsRunBase();
    testDrawRunScanAllowsRepeatedStreamDelta();
    testDrawRunScanAllowsMixedDirectAndIndexedNoDelta();
    testDrawRunScanAllowsLateIndexBufferDeltaInMixedRun();
    testIndexedDrawRunScanPreservesPerRecordParams();
    testIndexedDrawRunScanUsesFirstIndexBufferDeltaAsRunBase();
    testIndexedDrawRunScanAllowsRepeatedIndexBufferDelta();
    testIndexedDrawRunScanAllowsMixedDirectWithBaseIndexBuffer();
    testIndexedDrawRunScanAllowsDifferentIndexBufferDelta();
    testImportedRecordReplayInfoClassifiesOrderingBoundaries();
    testDrawRunHelpers();
  } catch (const dxmt9::d3d9::devicec::spec::TestFailure& e) {
    std::cerr << "chunk_record_replay_spec failed: " << e.what() << '\n';
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}
