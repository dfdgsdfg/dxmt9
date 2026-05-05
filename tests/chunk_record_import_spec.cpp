#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "device_c_record_utils.hpp"

namespace {

using dxmt9::d3d9::devicec::D9CCommandRecordValidationStatus;
using dxmt9::d3d9::devicec::ImportedChunkHandleSet;
using dxmt9::d3d9::devicec::ImportedChunkValidationStatus;
using dxmt9::d3d9::devicec::ImportedDrawRunScanStop;
using dxmt9::d3d9::devicec::ImportedReplayHazardState;
using dxmt9::d3d9::devicec::ImportedReplayOrderingAction;
using dxmt9::d3d9::devicec::ImportedRecordReplayCategory;
using dxmt9::d3d9::devicec::appendImportedChunkHandle;
using dxmt9::d3d9::devicec::collectImportedRecordResourceHandles;
using dxmt9::d3d9::devicec::collectImportedRecordResourceHazards;
using dxmt9::d3d9::devicec::evaluateImportedReplayOrdering;
using dxmt9::d3d9::devicec::makeImportedChunkView;
using dxmt9::d3d9::devicec::makeImportedChunkHandleEntries;
using dxmt9::d3d9::devicec::makeRunParam;
using dxmt9::d3d9::devicec::nextImportedRecord;
using dxmt9::d3d9::devicec::nextImportedReplayHazardState;
using dxmt9::d3d9::devicec::packetHasNoStateDelta;
using dxmt9::d3d9::devicec::replayInfoForCommandRecordType;
using dxmt9::d3d9::devicec::replayInfoForImportedRecord;
using dxmt9::d3d9::devicec::scanImportedDrawRun;
using dxmt9::d3d9::devicec::validateCommandRecord;
using dxmt9::d3d9::devicec::validateImportedChunk;

struct TestFailure : std::runtime_error {
  using std::runtime_error::runtime_error;
};

[[noreturn]] void fail(std::string message) {
  throw TestFailure(std::move(message));
}

void check(bool condition, std::string_view message) {
  if (!condition) {
    fail(std::string(message));
  }
}

template <typename A, typename B>
void checkEq(const A& left, const B& right, std::string_view message) {
  if (!(left == right)) {
    std::ostringstream out;
    out << message << " (" << left << " vs " << right << ")";
    fail(out.str());
  }
}

template <typename T>
std::vector<std::uint8_t> recordBytes(const T& record, std::size_t byteCount) {
  std::vector<std::uint8_t> bytes(byteCount);
  std::memcpy(bytes.data(), &record, std::min(sizeof(record), byteCount));
  return bytes;
}

template <typename T>
std::vector<std::uint8_t> recordBytes(const T& record) {
  return recordBytes(record, sizeof(record));
}

template <typename T>
void appendRecord(std::vector<std::uint8_t>& bytes, const T& record,
                  std::size_t byteCount) {
  auto recordData = recordBytes(record, byteCount);
  bytes.insert(bytes.end(), recordData.begin(), recordData.end());
}

template <typename T>
void appendRecord(std::vector<std::uint8_t>& bytes, const T& record) {
  appendRecord(bytes, record, sizeof(record));
}

D9CCommandRecordPresent makePresentRecord() {
  D9CCommandRecordPresent present{};
  present.header.type = D9C_COMMAND_RECORD_PRESENT;
  present.header.size = sizeof(present);
  return present;
}

D9CCommandRecordDrawPrimitive makeDrawPrimitiveRecord(
    std::uint32_t startVertex,
    std::uint32_t primitiveCount,
    bool hasStateDelta = false) {
  D9CCommandRecordDrawPrimitive draw{};
  draw.header.type = D9C_COMMAND_RECORD_DRAW_PRIMITIVE;
  draw.header.size = sizeof(draw);
  draw.packet.primitiveType = 4u;
  draw.packet.startVertex = startVertex;
  draw.packet.primitiveCount = primitiveCount;
  if (hasStateDelta) {
    draw.packet.renderStateCount = 1u;
    draw.packet.renderStates[0].state = 7u;
    draw.packet.renderStates[0].value = 9u;
  }
  return draw;
}

D9CWireHandle wireHandle(std::uint64_t value) {
  return D9CWireHandle{
      .lo = static_cast<std::uint32_t>(value),
      .hi = static_cast<std::uint32_t>(value >> 32),
  };
}

D9CCommandRecordDrawPrimitive makeHazardDrawRecord(
    std::uint64_t renderTarget,
    std::uint64_t texture = 0,
    std::uint64_t vertexBuffer = 0) {
  auto draw = makeDrawPrimitiveRecord(0u, 1u);
  draw.packet.rtMask = 0x1u;
  draw.packet.rtHandles[0] = wireHandle(renderTarget);
  if (texture != 0) {
    draw.packet.textureMask = 0x1u;
    draw.packet.textures[0] = wireHandle(texture);
  }
  if (vertexBuffer != 0) {
    draw.packet.streamSourceMask = 0x1u;
    draw.packet.streamSources[0].buffer = wireHandle(vertexBuffer);
  }
  return draw;
}

bool containsHandle(const std::vector<D9CChunkHandleEntry>& entries,
                    std::uint32_t kind,
                    std::uint64_t handle) {
  for (const auto& entry : entries) {
    if (entry.kind == kind && entry.handle == handle) {
      return true;
    }
  }
  return false;
}

void checkStatus(const std::vector<std::uint8_t>& bytes,
                 D9CCommandRecordValidationStatus expected,
                 std::string_view message) {
  const auto validation = validateCommandRecord(
      bytes.empty() ? nullptr : bytes.data(),
      static_cast<std::uint32_t>(bytes.size()));
  checkEq(static_cast<int>(validation.status), static_cast<int>(expected), message);
}

void testFixedRecordValidation() {
  D9CCommandRecordPresent present{};
  present.header.type = D9C_COMMAND_RECORD_PRESENT;
  present.header.size = sizeof(present);

  auto bytes = recordBytes(present);
  const auto validation = validateCommandRecord(
      bytes.data(), static_cast<std::uint32_t>(bytes.size()));
  check(validation.valid(), "present fixed record validates");
  checkEq(validation.minimumSize, static_cast<std::uint32_t>(sizeof(present)),
          "present fixed minimum size");
  checkEq(validation.expectedSize, static_cast<std::uint64_t>(sizeof(present)),
          "present fixed expected size");

  present.header.size = sizeof(present) + 4u;
  checkStatus(recordBytes(present, present.header.size),
              D9CCommandRecordValidationStatus::SizeMismatch,
              "fixed records reject trailing bytes");
}

void testClearRectTailValidation() {
  D9CCommandRecordClear clear{};
  clear.header.type = D9C_COMMAND_RECORD_CLEAR;
  clear.rectCount = 2u;
  clear.rectOffset = sizeof(D9CCommandRecordClear);
  clear.header.size = sizeof(D9CCommandRecordClear) + sizeof(D9CRect) * clear.rectCount;

  auto bytes = recordBytes(clear, clear.header.size);
  const auto validation = validateCommandRecord(
      bytes.data(), static_cast<std::uint32_t>(bytes.size()));
  check(validation.valid(), "clear rect tail validates");
  checkEq(validation.expectedSize, static_cast<std::uint64_t>(clear.header.size),
          "clear rect expected size includes tail");

  clear.rectOffset = sizeof(D9CCommandRecordHeader);
  checkStatus(recordBytes(clear, clear.header.size),
              D9CCommandRecordValidationStatus::SizeMismatch,
              "clear rejects non-tail rect offset");
}

void testSetConstTailValidation() {
  D9CCommandRecordSetConst setConst{};
  setConst.header.type = D9C_COMMAND_RECORD_SET_VS_CONST_F;
  setConst.count = 3u;
  setConst.header.size = sizeof(D9CCommandRecordSetConst) + sizeof(float) * 4u * setConst.count;

  auto bytes = recordBytes(setConst, setConst.header.size);
  const auto validation = validateCommandRecord(
      bytes.data(), static_cast<std::uint32_t>(bytes.size()));
  check(validation.valid(), "set const float tail validates");
  checkEq(validation.expectedSize, static_cast<std::uint64_t>(setConst.header.size),
          "set const float expected size includes vec4 tail");

  setConst.header.type = D9C_COMMAND_RECORD_SET_PS_CONST_B;
  setConst.count = 5u;
  setConst.header.size = sizeof(D9CCommandRecordSetConst) +
                         sizeof(std::uint32_t) * setConst.count;
  bytes = recordBytes(setConst, setConst.header.size);
  const auto boolValidation = validateCommandRecord(
      bytes.data(), static_cast<std::uint32_t>(bytes.size()));
  check(boolValidation.valid(), "set const bool tail validates");
  checkEq(boolValidation.expectedSize, static_cast<std::uint64_t>(setConst.header.size),
          "set const bool expected size uses scalar tail");

  setConst.header.type = D9C_COMMAND_RECORD_SET_VS_CONST_F;
  setConst.count = 3u;
  setConst.header.size = sizeof(D9CCommandRecordSetConst) + sizeof(float) * 4u * 2u;
  checkStatus(recordBytes(setConst, setConst.header.size),
              D9CCommandRecordValidationStatus::SizeMismatch,
              "set const rejects truncated logical tail");
}

void testInvalidTruncatedAndUnknownRecords() {
  std::vector<std::uint8_t> shortHeader(sizeof(D9CCommandRecordHeader) - 1u);
  checkStatus(shortHeader, D9CCommandRecordValidationStatus::TruncatedHeader,
              "short header is rejected");

  D9CCommandRecordHeader tinySize{};
  tinySize.type = D9C_COMMAND_RECORD_PRESENT;
  tinySize.size = sizeof(D9CCommandRecordHeader) - 1u;
  checkStatus(recordBytes(tinySize), D9CCommandRecordValidationStatus::SizeTooSmall,
              "header size smaller than common header is rejected");

  D9CCommandRecordClear clear{};
  clear.header.type = D9C_COMMAND_RECORD_CLEAR;
  clear.rectCount = 1u;
  clear.rectOffset = sizeof(D9CCommandRecordClear);
  clear.header.size = sizeof(D9CCommandRecordClear) + sizeof(D9CRect);
  checkStatus(recordBytes(clear, clear.header.size - 1u),
              D9CCommandRecordValidationStatus::TruncatedRecord,
              "available bytes shorter than header size is rejected");

  D9CCommandRecordHeader unknown{};
  unknown.type = 0xffffu;
  unknown.size = sizeof(unknown);
  checkStatus(recordBytes(unknown), D9CCommandRecordValidationStatus::UnknownType,
              "unknown record type is rejected");
}

void testImportedMultiRecordIteration() {
  const auto present = makePresentRecord();
  const auto draw = makeDrawPrimitiveRecord(4u, 2u);

  std::vector<std::uint8_t> bytes;
  appendRecord(bytes, present);
  appendRecord(bytes, draw);

  const auto chunk = makeImportedChunkView(
      bytes.data(), static_cast<std::uint32_t>(bytes.size()), 2u);

  const auto first = nextImportedRecord(chunk, 0u, 0u);
  check(first.has_value(), "first imported record exists");
  check(first->valid(), "first imported record validates");
  check(first->record == bytes.data(), "first imported record points at stream start");
  checkEq(first->offset, 0u, "first imported record offset");
  checkEq(first->index, 0u, "first imported record index");
  checkEq(first->header.type, static_cast<std::uint32_t>(D9C_COMMAND_RECORD_PRESENT),
          "first imported record type");
  checkEq(first->header.size, static_cast<std::uint32_t>(sizeof(present)),
          "first imported record size");

  const auto second = nextImportedRecord(chunk, first->nextOffset(), first->nextIndex());
  check(second.has_value(), "second imported record exists");
  check(second->valid(), "second imported record validates");
  check(second->record == bytes.data() + sizeof(present),
        "second imported record points at second byte range");
  checkEq(second->offset, static_cast<std::uint32_t>(sizeof(present)),
          "second imported record offset");
  checkEq(second->index, 1u, "second imported record index");
  checkEq(second->header.type,
          static_cast<std::uint32_t>(D9C_COMMAND_RECORD_DRAW_PRIMITIVE),
          "second imported record type");

  const auto end = nextImportedRecord(chunk, second->nextOffset(), second->nextIndex());
  check(!end.has_value(), "imported record iteration stops at chunk end");

  const auto validation = validateImportedChunk(chunk);
  check(validation.valid(), "valid imported chunk validates");
  checkEq(validation.consumedBytes, static_cast<std::uint32_t>(bytes.size()),
          "valid imported chunk consumes all bytes");
  checkEq(validation.parsedRecordCount, 2u, "valid imported chunk parses count");
}

void testImportedRecordCountMismatch() {
  const auto present = makePresentRecord();
  const auto draw = makeDrawPrimitiveRecord(0u, 1u);

  std::vector<std::uint8_t> bytes;
  appendRecord(bytes, present);
  appendRecord(bytes, draw);

  const auto chunk = makeImportedChunkView(
      bytes.data(), static_cast<std::uint32_t>(bytes.size()), 1u);
  const auto validation = validateImportedChunk(chunk);
  checkEq(static_cast<int>(validation.status),
          static_cast<int>(ImportedChunkValidationStatus::RecordCountMismatch),
          "record count mismatch is reported");
  checkEq(validation.parsedRecordCount, 2u,
          "record count mismatch reports parsed records");
  checkEq(validation.consumedBytes, static_cast<std::uint32_t>(bytes.size()),
          "record count mismatch still consumes valid byte stream");
}

void testImportedTruncatedTail() {
  const auto present = makePresentRecord();

  D9CCommandRecordClear clear{};
  clear.header.type = D9C_COMMAND_RECORD_CLEAR;
  clear.rectCount = 1u;
  clear.rectOffset = sizeof(D9CCommandRecordClear);
  clear.header.size = sizeof(D9CCommandRecordClear) + sizeof(D9CRect);

  std::vector<std::uint8_t> bytes;
  appendRecord(bytes, present);
  appendRecord(bytes, clear, clear.header.size - 1u);

  const auto chunk = makeImportedChunkView(
      bytes.data(), static_cast<std::uint32_t>(bytes.size()), 2u);
  const auto validation = validateImportedChunk(chunk);
  checkEq(static_cast<int>(validation.status),
          static_cast<int>(ImportedChunkValidationStatus::InvalidRecord),
          "truncated tail marks chunk invalid");
  checkEq(validation.parsedRecordCount, 1u,
          "truncated tail reports records before failure");
  checkEq(validation.consumedBytes, static_cast<std::uint32_t>(sizeof(present)),
          "truncated tail reports failure offset");
  checkEq(validation.failedRecord.offset, static_cast<std::uint32_t>(sizeof(present)),
          "truncated tail failed record offset");
  checkEq(validation.failedRecord.index, 1u,
          "truncated tail failed record index");
  checkEq(static_cast<int>(validation.failedRecord.validation.status),
          static_cast<int>(D9CCommandRecordValidationStatus::TruncatedRecord),
          "truncated tail preserves record validation status");
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

void testResourceRetentionDerivationFromDrawRecords() {
  D9CCommandRecordDrawIndexedPrimitive draw{};
  draw.header.type = D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE;
  draw.header.size = sizeof(draw);
  draw.packet.state.textureMask = 0x3u;
  draw.packet.state.textures[0] = wireHandle(0x1000u);
  draw.packet.state.textures[1] = wireHandle(0x1000u);
  draw.packet.state.streamSourceMask = 0x3u;
  draw.packet.state.streamSources[0].buffer = wireHandle(0x2000u);
  draw.packet.state.streamSources[1].buffer = wireHandle(0x2008u);
  draw.packet.state.rtMask = 0x3u;
  draw.packet.state.rtHandles[0] = wireHandle(0x3000u);
  draw.packet.state.rtHandles[1] = wireHandle(0x3008u);
  draw.packet.state.dsValid = 1u;
  draw.packet.state.dsHandle = wireHandle(0x3010u);
  draw.packet.ibValid = 1u;
  draw.packet.ibHandle = wireHandle(0x2010u);

  const auto bytes = recordBytes(draw);
  const auto chunk = makeImportedChunkView(bytes.data(), static_cast<std::uint32_t>(bytes.size()), 1u);
  const auto record = nextImportedRecord(chunk, 0u, 0u);
  check(record.has_value(), "draw retention record exists");
  check(record->valid(), "draw retention record validates");

  ImportedChunkHandleSet handles;
  collectImportedRecordResourceHandles(*record, handles);
  const auto entries = makeImportedChunkHandleEntries(handles);

  checkEq(entries.size(), static_cast<std::size_t>(7), "draw retention set deduplicates handles");
  check(containsHandle(entries, D9C_CHUNK_HANDLE_KIND_TEXTURE, 0x1000u),
        "draw retention includes texture handle");
  check(containsHandle(entries, D9C_CHUNK_HANDLE_KIND_BUFFER, 0x2000u),
        "draw retention includes stream 0 buffer");
  check(containsHandle(entries, D9C_CHUNK_HANDLE_KIND_BUFFER, 0x2008u),
        "draw retention includes stream 1 buffer");
  check(containsHandle(entries, D9C_CHUNK_HANDLE_KIND_BUFFER, 0x2010u),
        "draw retention includes index buffer");
  check(containsHandle(entries, D9C_CHUNK_HANDLE_KIND_SURFACE, 0x3000u),
        "draw retention includes RT0");
  check(containsHandle(entries, D9C_CHUNK_HANDLE_KIND_SURFACE, 0x3008u),
        "draw retention includes RT1");
  check(containsHandle(entries, D9C_CHUNK_HANDLE_KIND_SURFACE, 0x3010u),
        "draw retention includes depth-stencil");
}

void testResourceRetentionDerivationFromSurfaceAndReadbackRecords() {
  D9CCommandRecordStretchRect stretch{};
  stretch.header.type = D9C_COMMAND_RECORD_STRETCH_RECT;
  stretch.header.size = sizeof(stretch);
  stretch.srcWire = 0x4000u;
  stretch.dstWire = 0x4008u;

  D9CCommandRecordUpdateTexture updateTexture{};
  updateTexture.header.type = D9C_COMMAND_RECORD_UPDATE_TEXTURE;
  updateTexture.header.size = sizeof(updateTexture);
  updateTexture.srcWire = 0x5000u;
  updateTexture.dstWire = 0x5008u;

  D9CCommandRecordReadback readback{};
  readback.header.type = D9C_COMMAND_RECORD_READBACK;
  readback.header.size = sizeof(readback);
  readback.srcWire = 0x6000u;
  readback.dstWire = 0x6008u;

  std::vector<std::uint8_t> bytes;
  appendRecord(bytes, stretch);
  appendRecord(bytes, updateTexture);
  appendRecord(bytes, readback);

  const auto chunk = makeImportedChunkView(bytes.data(), static_cast<std::uint32_t>(bytes.size()), 3u);
  ImportedChunkHandleSet handles;
  std::uint32_t offset = 0;
  std::uint32_t index = 0;
  while (auto record = nextImportedRecord(chunk, offset, index)) {
    check(record->valid(), "surface/readback retention record validates");
    collectImportedRecordResourceHandles(*record, handles);
    const auto info = replayInfoForImportedRecord(*record);
    check(info.barrier, "surface/readback record is an ordering barrier");
    offset = record->nextOffset();
    index = record->nextIndex();
  }
  checkEq(index, 3u, "surface/readback retention parsed all records");

  const auto entries = makeImportedChunkHandleEntries(handles);
  checkEq(entries.size(), static_cast<std::size_t>(6), "surface/readback retention includes all unique resources");
  check(containsHandle(entries, D9C_CHUNK_HANDLE_KIND_SURFACE, 0x4000u), "stretch source retained");
  check(containsHandle(entries, D9C_CHUNK_HANDLE_KIND_SURFACE, 0x4008u), "stretch destination retained");
  check(containsHandle(entries, D9C_CHUNK_HANDLE_KIND_TEXTURE, 0x5000u), "update texture source retained");
  check(containsHandle(entries, D9C_CHUNK_HANDLE_KIND_TEXTURE, 0x5008u), "update texture destination retained");
  check(containsHandle(entries, D9C_CHUNK_HANDLE_KIND_SURFACE, 0x6000u), "readback source retained");
  check(containsHandle(entries, D9C_CHUNK_HANDLE_KIND_SURFACE, 0x6008u), "readback destination retained");
}

void testImportedRecordHazardsSeparateReadsAndWrites() {
  auto draw = makeHazardDrawRecord(0x3000u, 0x1000u, 0x2000u);
  D9CCommandRecordDrawIndexedPrimitive indexed{};
  indexed.header.type = D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE;
  indexed.header.size = sizeof(indexed);
  indexed.packet.state = draw.packet;
  indexed.packet.ibValid = 1u;
  indexed.packet.ibHandle = wireHandle(0x2010u);

  const auto bytes = recordBytes(indexed);
  const auto chunk = makeImportedChunkView(bytes.data(), static_cast<std::uint32_t>(bytes.size()), 1u);
  const auto record = nextImportedRecord(chunk, 0u, 0u);
  check(record.has_value(), "hazard record exists");
  check(record->valid(), "hazard record validates");

  dxmt9::d3d9::devicec::ImportedRecordResourceHazards hazards;
  collectImportedRecordResourceHazards(*record, hazards);
  const auto reads = makeImportedChunkHandleEntries(hazards.reads);
  const auto writes = makeImportedChunkHandleEntries(hazards.writes);

  checkEq(reads.size(), static_cast<std::size_t>(3), "draw hazards track read handles");
  check(containsHandle(reads, D9C_CHUNK_HANDLE_KIND_TEXTURE, 0x1000u),
        "draw hazard reads texture");
  check(containsHandle(reads, D9C_CHUNK_HANDLE_KIND_BUFFER, 0x2000u),
        "draw hazard reads stream buffer");
  check(containsHandle(reads, D9C_CHUNK_HANDLE_KIND_BUFFER, 0x2010u),
        "indexed draw hazard reads index buffer");
  checkEq(writes.size(), static_cast<std::size_t>(1), "draw hazards track write handles");
  check(containsHandle(writes, D9C_CHUNK_HANDLE_KIND_SURFACE, 0x3000u),
        "draw hazard writes render target");
}

void testImportedReplayOrderingDecisionsAreDataDriven() {
  const auto firstDraw = makeHazardDrawRecord(0x3000u, 0x1000u, 0x2000u);
  const auto cleanDraw = makeHazardDrawRecord(0x3008u, 0x1008u, 0x2008u);
  const auto overlappingDraw = makeHazardDrawRecord(0x3000u, 0x1010u, 0x2010u);

  D9CCommandRecordClear clear{};
  clear.header.type = D9C_COMMAND_RECORD_CLEAR;
  clear.header.size = sizeof(clear);
  clear.rectOffset = sizeof(D9CCommandRecordClear);

  D9CCommandRecordReadback readback{};
  readback.header.type = D9C_COMMAND_RECORD_READBACK;
  readback.header.size = sizeof(readback);
  readback.srcWire = 0x3000u;
  readback.dstWire = 0x3010u;

  std::vector<std::uint8_t> bytes;
  appendRecord(bytes, firstDraw);
  appendRecord(bytes, cleanDraw);
  appendRecord(bytes, overlappingDraw);
  appendRecord(bytes, clear);
  appendRecord(bytes, firstDraw);
  appendRecord(bytes, readback);

  const auto chunk = makeImportedChunkView(bytes.data(), static_cast<std::uint32_t>(bytes.size()), 6u);
  ImportedReplayHazardState state;
  std::uint32_t offset = 0;
  std::uint32_t index = 0;

  const auto nextDecision = [&]() {
    const auto record = nextImportedRecord(chunk, offset, index);
    check(record.has_value(), "ordering test record exists");
    check(record->valid(), "ordering test record validates");
    const auto decision = evaluateImportedReplayOrdering(*record, state);
    state = nextImportedReplayHazardState(state, decision);
    offset = record->nextOffset();
    index = record->nextIndex();
    return decision;
  };

  auto decision = nextDecision();
  checkEq(static_cast<int>(decision.action),
          static_cast<int>(ImportedReplayOrderingAction::Continue),
          "first draw starts active hazard tracking");
  check(!decision.readAfterWrite && !decision.writeAfterRead && !decision.writeAfterWrite,
        "first draw has no prior hazards");
  check(state.active, "first draw leaves active hazards");

  decision = nextDecision();
  checkEq(static_cast<int>(decision.action),
          static_cast<int>(ImportedReplayOrderingAction::Continue),
          "disjoint draw continues without a boundary");
  check(!decision.hazardBoundary(), "disjoint draw is not a hazard boundary");

  decision = nextDecision();
  checkEq(static_cast<int>(decision.action),
          static_cast<int>(ImportedReplayOrderingAction::HazardBoundary),
          "overlapping render-target write starts a hazard boundary");
  check(decision.writeAfterWrite, "overlapping draw reports WAW");
  check(decision.hazardBoundary(), "overlapping draw exposes hazard boundary helper");
  check(state.active, "hazard boundary starts a new active hazard scope");

  decision = nextDecision();
  checkEq(static_cast<int>(decision.action),
          static_cast<int>(ImportedReplayOrderingAction::BarrierBoundary),
          "clear is an explicit replay barrier");
  check(decision.barrierBoundary(), "clear exposes barrier boundary helper");
  check(decision.resetsActiveHazards, "barrier resets active hazards");
  check(!state.active, "barrier clears active hazard scope");

  decision = nextDecision();
  checkEq(static_cast<int>(decision.action),
          static_cast<int>(ImportedReplayOrderingAction::Continue),
          "draw after barrier does not inherit prior hazards");
  check(!decision.writeAfterWrite, "barrier prevents stale WAW carry-over");

  decision = nextDecision();
  checkEq(static_cast<int>(decision.action),
          static_cast<int>(ImportedReplayOrderingAction::SynchronousReadBoundary),
          "readback is a synchronous read boundary");
  check(decision.barrierBoundary(), "readback is also a barrier boundary");
  check(decision.resetsActiveHazards, "readback resets active hazards");
  check(!state.active, "readback clears active hazard scope");
  checkEq(index, 6u, "ordering decisions consumed all records");
}

void testImportedChunkHandleAppendRejectsInvalidAndDuplicateHandles() {
  ImportedChunkHandleSet handles;
  check(!appendImportedChunkHandle(handles, D9C_CHUNK_HANDLE_KIND_TEXTURE, 0u),
        "zero handle is ignored");
  check(!appendImportedChunkHandle(handles, D9C_CHUNK_HANDLE_KIND_VERTEX_DECL + 1u, 0x1u),
        "unknown handle kind is ignored");
  check(appendImportedChunkHandle(handles, D9C_CHUNK_HANDLE_KIND_BUFFER, 0x7000u),
        "first valid handle is appended");
  check(!appendImportedChunkHandle(handles, D9C_CHUNK_HANDLE_KIND_BUFFER, 0x7000u),
        "duplicate handle is ignored");

  const auto entries = makeImportedChunkHandleEntries(handles);
  checkEq(entries.size(), static_cast<std::size_t>(1), "only one valid handle retained");
  check(containsHandle(entries, D9C_CHUNK_HANDLE_KIND_BUFFER, 0x7000u),
        "valid handle entry retained");

  D9CCommandChunkWireHandleEntry wireEntry{};
  wireEntry.kind = entries[0].kind;
  wireEntry.opaqueHandle = entries[0].handle;
  checkEq(wireEntry.kind, D9C_CHUNK_HANDLE_KIND_BUFFER,
          "DOD wire handle entry preserves imported handle kind");
  checkEq(wireEntry.opaqueHandle, 0x7000u,
          "DOD wire handle entry preserves imported opaque handle");
  checkEq(wireEntry.generation, D9C_COMMAND_CHUNK_WIRE_HANDLE_GENERATION_NONE,
          "DOD wire handle generation defaults to none for imported legacy handles");
  checkEq(wireEntry.reserved0, 0u, "DOD wire handle reserved0 defaults to zero");
  checkEq(wireEntry.reserved1, 0u, "DOD wire handle reserved1 defaults to zero");
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
    testFixedRecordValidation();
    testClearRectTailValidation();
    testSetConstTailValidation();
    testInvalidTruncatedAndUnknownRecords();
    testImportedMultiRecordIteration();
    testImportedRecordCountMismatch();
    testImportedTruncatedTail();
    testDrawRunScanBoundary();
    testImportedRecordReplayInfoClassifiesOrderingBoundaries();
    testResourceRetentionDerivationFromDrawRecords();
    testResourceRetentionDerivationFromSurfaceAndReadbackRecords();
    testImportedRecordHazardsSeparateReadsAndWrites();
    testImportedReplayOrderingDecisionsAreDataDriven();
    testImportedChunkHandleAppendRejectsInvalidAndDuplicateHandles();
    testDrawRunHelpers();
  } catch (const TestFailure& e) {
    std::cerr << "chunk_record_import_spec failed: " << e.what() << '\n';
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}
