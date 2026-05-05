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
using dxmt9::d3d9::devicec::ImportedWireChunkValidationStatus;
using dxmt9::d3d9::devicec::appendImportedChunkHandle;
using dxmt9::d3d9::devicec::collectImportedWireChunkHandles;
using dxmt9::d3d9::devicec::collectImportedWireRecordHandles;
using dxmt9::d3d9::devicec::collectImportedRecordResourceHandles;
using dxmt9::d3d9::devicec::collectImportedRecordResourceHazards;
using dxmt9::d3d9::devicec::evaluateImportedReplayOrdering;
using dxmt9::d3d9::devicec::makeImportedChunkView;
using dxmt9::d3d9::devicec::makeImportedChunkHandleEntries;
using dxmt9::d3d9::devicec::makeImportedWireChunkBlobView;
using dxmt9::d3d9::devicec::makeImportedWireChunkView;
using dxmt9::d3d9::devicec::makeImportedWireRecordHandleView;
using dxmt9::d3d9::devicec::makeRunParam;
using dxmt9::d3d9::devicec::nextImportedRecord;
using dxmt9::d3d9::devicec::nextImportedReplayHazardState;
using dxmt9::d3d9::devicec::packetHasNoStateDelta;
using dxmt9::d3d9::devicec::replayInfoForCommandRecordType;
using dxmt9::d3d9::devicec::replayInfoForImportedRecord;
using dxmt9::d3d9::devicec::scanImportedDrawRun;
using dxmt9::d3d9::devicec::validateCommandRecord;
using dxmt9::d3d9::devicec::validateImportedChunk;
using dxmt9::d3d9::devicec::validateImportedWireChunk;

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

template <typename T>
void writeObject(std::vector<std::uint8_t>& bytes,
                 std::uint32_t offset,
                 const T& object) {
  const auto end = static_cast<std::size_t>(offset) + sizeof(object);
  if (bytes.size() < end) {
    bytes.resize(end);
  }
  std::memcpy(bytes.data() + offset, &object, sizeof(object));
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

D9CCommandRecordDrawIndexedPrimitive makeDrawIndexedPrimitiveRecord(
    std::uint32_t startIndex,
    std::uint32_t primitiveCount,
    bool hasStateDelta = false) {
  D9CCommandRecordDrawIndexedPrimitive draw{};
  draw.header.type = D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE;
  draw.header.size = sizeof(draw);
  draw.packet.state.primitiveType = 4u;
  draw.packet.startIndex = startIndex;
  draw.packet.primitiveCount = primitiveCount;
  draw.packet.numVertices = primitiveCount * 3u;
  if (hasStateDelta) {
    draw.packet.state.renderStateCount = 1u;
    draw.packet.state.renderStates[0].state = 7u;
    draw.packet.state.renderStates[0].value = 9u;
  }
  return draw;
}

D9CCommandRecordDrawPrimitiveUP makeDrawPrimitiveUPRecord(
    std::uint32_t vertexBytes = 48u) {
  D9CCommandRecordDrawPrimitiveUP draw{};
  draw.header.type = D9C_COMMAND_RECORD_DRAW_PRIMITIVE_UP;
  draw.header.size = static_cast<std::uint32_t>(
      sizeof(D9CCommandRecordDrawPrimitiveUP) + vertexBytes);
  draw.packet.state.primitiveType = 4u;
  draw.packet.primitiveCount = 1u;
  draw.packet.stride = 16u;
  draw.packet.vertexDataOffset =
      static_cast<std::uint32_t>(sizeof(D9CCommandRecordDrawPrimitiveUP));
  draw.packet.vertexDataSize = vertexBytes;
  return draw;
}

D9CCommandRecordDrawIndexedPrimitiveUP makeDrawIndexedPrimitiveUPRecord(
    std::uint32_t indexBytes = 6u,
    std::uint32_t vertexBytes = 48u) {
  D9CCommandRecordDrawIndexedPrimitiveUP draw{};
  draw.header.type = D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE_UP;
  draw.header.size = static_cast<std::uint32_t>(
      sizeof(D9CCommandRecordDrawIndexedPrimitiveUP) + indexBytes + vertexBytes);
  draw.packet.state.primitiveType = 4u;
  draw.packet.minVertex = 0u;
  draw.packet.numVertices = 3u;
  draw.packet.primitiveCount = 1u;
  draw.packet.indexFormat = 101u;
  draw.packet.stride = 16u;
  draw.packet.indexDataOffset =
      static_cast<std::uint32_t>(sizeof(D9CCommandRecordDrawIndexedPrimitiveUP));
  draw.packet.indexDataSize = indexBytes;
  draw.packet.vertexDataOffset = draw.packet.indexDataOffset + indexBytes;
  draw.packet.vertexDataSize = vertexBytes;
  return draw;
}

D9CCommandRecordSetConst makeSetConstRecord(
    std::uint32_t type,
    std::uint32_t count,
    std::uint32_t elementSize) {
  D9CCommandRecordSetConst setConst{};
  setConst.header.type = type;
  setConst.header.size = static_cast<std::uint32_t>(
      sizeof(D9CCommandRecordSetConst) +
      static_cast<std::uint64_t>(count) * elementSize);
  setConst.start = 2u;
  setConst.count = count;
  return setConst;
}

D9CCommandRecordClear makeClearRecord(std::uint32_t rectCount = 0u) {
  D9CCommandRecordClear clear{};
  clear.header.type = D9C_COMMAND_RECORD_CLEAR;
  clear.header.size = static_cast<std::uint32_t>(
      sizeof(D9CCommandRecordClear) + sizeof(D9CRect) * rectCount);
  clear.flags = 1u;
  clear.colorARGB = 0xff00ff00u;
  clear.z = 1.0f;
  clear.rectCount = rectCount;
  clear.rectOffset = sizeof(D9CCommandRecordClear);
  return clear;
}

D9CCommandRecordStretchRect makeStretchRectRecord() {
  D9CCommandRecordStretchRect stretch{};
  stretch.header.type = D9C_COMMAND_RECORD_STRETCH_RECT;
  stretch.header.size = sizeof(stretch);
  stretch.srcWire = 0x4000u;
  stretch.dstWire = 0x4008u;
  stretch.hasSrcRect = 1u;
  stretch.hasDstRect = 1u;
  stretch.filter = 1u;
  stretch.srcRect = D9CRect{0, 0, 4, 4};
  stretch.dstRect = D9CRect{1, 1, 5, 5};
  return stretch;
}

D9CCommandRecordColorFill makeColorFillRecord() {
  D9CCommandRecordColorFill color{};
  color.header.type = D9C_COMMAND_RECORD_COLOR_FILL;
  color.header.size = sizeof(color);
  color.surfaceWire = 0x4100u;
  color.colorARGB = 0xff0000ffu;
  color.hasRect = 1u;
  color.rect = D9CRect{0, 0, 8, 8};
  return color;
}

D9CCommandRecordUpdateTexture makeUpdateTextureRecord() {
  D9CCommandRecordUpdateTexture update{};
  update.header.type = D9C_COMMAND_RECORD_UPDATE_TEXTURE;
  update.header.size = sizeof(update);
  update.srcWire = 0x5000u;
  update.dstWire = 0x5008u;
  return update;
}

D9CCommandRecordUpdateSurface makeUpdateSurfaceRecord() {
  D9CCommandRecordUpdateSurface update{};
  update.header.type = D9C_COMMAND_RECORD_UPDATE_SURFACE;
  update.header.size = sizeof(update);
  update.srcWire = 0x5100u;
  update.dstWire = 0x5108u;
  update.hasSrcRect = 1u;
  update.hasDstPoint = 1u;
  update.srcRect = D9CRect{0, 0, 16, 16};
  update.dstPoint = D9CRect{2, 3, 2, 3};
  return update;
}

D9CCommandRecordQueryIssue makeQueryIssueRecord() {
  D9CCommandRecordQueryIssue query{};
  query.header.type = D9C_COMMAND_RECORD_QUERY_ISSUE;
  query.header.size = sizeof(query);
  query.queryWire = 0x5200u;
  query.flags = 1u;
  return query;
}

D9CCommandRecordReadback makeReadbackRecord() {
  D9CCommandRecordReadback readback{};
  readback.header.type = D9C_COMMAND_RECORD_READBACK;
  readback.header.size = sizeof(readback);
  readback.srcWire = 0x6000u;
  readback.dstWire = 0x6008u;
  return readback;
}

D9CCommandRecordApplyState makeApplyStateRecord() {
  D9CCommandRecordApplyState apply{};
  apply.header.type = D9C_COMMAND_RECORD_APPLY_STATE;
  apply.header.size = sizeof(apply);
  apply.packet.renderStateCount = 1u;
  apply.packet.renderStates[0].state = 7u;
  apply.packet.renderStates[0].value = 9u;
  return apply;
}

D9CWireHandle wireHandle(std::uint64_t value) {
  return D9CWireHandle{
      .lo = static_cast<std::uint32_t>(value),
      .hi = static_cast<std::uint32_t>(value >> 32),
  };
}

D9CCommandChunkWireHandleEntry wireHandleEntry(
    std::uint32_t kind,
    std::uint64_t handle,
    std::uint32_t reserved0 = 0u,
    std::uint32_t generation = D9C_COMMAND_CHUNK_WIRE_HANDLE_GENERATION_NONE,
    std::uint32_t reserved1 = 0u) {
  return D9CCommandChunkWireHandleEntry{
      .kind = kind,
      .generation = generation,
      .opaqueHandle = handle,
      .reserved0 = reserved0,
      .reserved1 = reserved1,
  };
}

D9CCommandChunkWireRecordHeader wireRecordHeader(
    std::uint32_t type,
    std::uint32_t payloadOffset,
    std::uint32_t payloadSize,
    std::uint32_t firstHandle = 0u,
    std::uint32_t handleCount = 0u) {
  return D9CCommandChunkWireRecordHeader{
      .type = type,
      .flags = D9C_COMMAND_CHUNK_WIRE_RECORD_FLAG_NONE,
      .payloadOffset = payloadOffset,
      .payloadSize = payloadSize,
      .firstHandle = firstHandle,
      .handleCount = handleCount,
      .reserved0 = 0u,
      .reserved1 = 0u,
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

template <typename T>
std::vector<std::uint8_t> declaredRecordBytes(const T& record) {
  return recordBytes(record, record.header.size);
}

void checkValidRecordBytes(const std::vector<std::uint8_t>& bytes,
                           std::uint32_t expectedType,
                           std::uint32_t expectedMinimumSize,
                           std::uint64_t expectedSize,
                           std::string_view message) {
  const auto validation = validateCommandRecord(
      bytes.empty() ? nullptr : bytes.data(),
      static_cast<std::uint32_t>(bytes.size()));
  check(validation.valid(), message);
  checkEq(validation.header.type, expectedType, std::string(message) + " type");
  checkEq(validation.minimumSize, expectedMinimumSize,
          std::string(message) + " minimum size");
  checkEq(validation.expectedSize, expectedSize,
          std::string(message) + " expected size");
}

template <typename T>
void checkExactRecordSizeMatrix(const T& validRecord,
                                std::string_view name) {
  checkValidRecordBytes(
      recordBytes(validRecord),
      validRecord.header.type,
      static_cast<std::uint32_t>(sizeof(T)),
      sizeof(T),
      std::string(name) + " exact record validates");

  auto tooSmall = validRecord;
  tooSmall.header.size = static_cast<std::uint32_t>(sizeof(T) - 1u);
  checkStatus(recordBytes(tooSmall, tooSmall.header.size),
              D9CCommandRecordValidationStatus::SizeTooSmall,
              std::string(name) + " rejects undersized record");

  auto trailing = validRecord;
  trailing.header.size = static_cast<std::uint32_t>(sizeof(T) + 4u);
  checkStatus(recordBytes(trailing, trailing.header.size),
              D9CCommandRecordValidationStatus::SizeMismatch,
              std::string(name) + " rejects trailing bytes");

  auto truncated = validRecord;
  truncated.header.size = static_cast<std::uint32_t>(sizeof(T) + 4u);
  checkStatus(recordBytes(truncated, sizeof(T)),
              D9CCommandRecordValidationStatus::TruncatedRecord,
              std::string(name) + " rejects unavailable declared bytes");
}

void checkWireRecordMatrix(const std::vector<std::uint8_t>& payload,
                           std::uint32_t type,
                           std::string_view name) {
  std::vector<D9CCommandChunkWireHandleEntry> handles{
      wireHandleEntry(D9C_CHUNK_HANDLE_KIND_SURFACE, 0x7000u),
  };
  std::vector<D9CCommandChunkWireRecordHeader> records{
      wireRecordHeader(type, 0u, static_cast<std::uint32_t>(payload.size()),
                       0u, 1u),
  };

  auto wire = makeImportedWireChunkView(
      records.data(), static_cast<std::uint32_t>(records.size()),
      payload.data(), static_cast<std::uint32_t>(payload.size()),
      handles.data(), static_cast<std::uint32_t>(handles.size()));
  auto validation = validateImportedWireChunk(wire);
  check(validation.valid(), std::string(name) + " validates through wire view");
  checkEq(validation.parsedRecordCount, 1u,
          std::string(name) + " wire validation parses one record");

  records[0] = wireRecordHeader(type, static_cast<std::uint32_t>(payload.size()),
                                1u, 0u, 1u);
  wire = makeImportedWireChunkView(
      records.data(), static_cast<std::uint32_t>(records.size()),
      payload.data(), static_cast<std::uint32_t>(payload.size()),
      handles.data(), static_cast<std::uint32_t>(handles.size()));
  validation = validateImportedWireChunk(wire);
  checkEq(static_cast<int>(validation.status),
          static_cast<int>(ImportedWireChunkValidationStatus::InvalidRecordRange),
          std::string(name) + " rejects payload range past arena");

  auto oversizedPayload = payload;
  oversizedPayload.push_back(0u);
  records[0] = wireRecordHeader(type, 0u,
                                static_cast<std::uint32_t>(oversizedPayload.size()),
                                0u, 1u);
  wire = makeImportedWireChunkView(
      records.data(), static_cast<std::uint32_t>(records.size()),
      oversizedPayload.data(), static_cast<std::uint32_t>(oversizedPayload.size()),
      handles.data(), static_cast<std::uint32_t>(handles.size()));
  validation = validateImportedWireChunk(wire);
  checkEq(static_cast<int>(validation.status),
          static_cast<int>(ImportedWireChunkValidationStatus::InvalidRecord),
          std::string(name) + " rejects payload size larger than record size");
  checkEq(static_cast<int>(validation.failedRecord.validation.status),
          static_cast<int>(D9CCommandRecordValidationStatus::SizeMismatch),
          std::string(name) + " reports record size mismatch");

  records[0] = wireRecordHeader(D9C_COMMAND_RECORD_PRESENT, 0u,
                                static_cast<std::uint32_t>(payload.size()),
                                0u, 1u);
  wire = makeImportedWireChunkView(
      records.data(), static_cast<std::uint32_t>(records.size()),
      payload.data(), static_cast<std::uint32_t>(payload.size()),
      handles.data(), static_cast<std::uint32_t>(handles.size()));
  validation = validateImportedWireChunk(wire);
  checkEq(static_cast<int>(validation.status),
          static_cast<int>(ImportedWireChunkValidationStatus::InvalidRecord),
          std::string(name) + " rejects wire type/payload type mismatch");
  checkEq(validation.failedRecord.header.type, type,
          std::string(name) + " mismatch preserves decoded payload type");

  records[0] = wireRecordHeader(type, 0u,
                                static_cast<std::uint32_t>(payload.size()),
                                0u, 1u);
  records[0].reserved0 = 1u;
  wire = makeImportedWireChunkView(
      records.data(), static_cast<std::uint32_t>(records.size()),
      payload.data(), static_cast<std::uint32_t>(payload.size()),
      handles.data(), static_cast<std::uint32_t>(handles.size()));
  validation = validateImportedWireChunk(wire);
  checkEq(static_cast<int>(validation.status),
          static_cast<int>(ImportedWireChunkValidationStatus::InvalidRecordHeader),
          std::string(name) + " rejects nonzero wire record reserved fields");

  records[0].reserved0 = 0u;
  records[0].flags = 1u;
  wire = makeImportedWireChunkView(
      records.data(), static_cast<std::uint32_t>(records.size()),
      payload.data(), static_cast<std::uint32_t>(payload.size()),
      handles.data(), static_cast<std::uint32_t>(handles.size()));
  validation = validateImportedWireChunk(wire);
  checkEq(static_cast<int>(validation.status),
          static_cast<int>(ImportedWireChunkValidationStatus::InvalidRecordHeader),
          std::string(name) + " rejects unsupported wire record flags");

  records[0] = wireRecordHeader(type, 0u,
                                static_cast<std::uint32_t>(payload.size()),
                                1u, 1u);
  wire = makeImportedWireChunkView(
      records.data(), static_cast<std::uint32_t>(records.size()),
      payload.data(), static_cast<std::uint32_t>(payload.size()),
      handles.data(), static_cast<std::uint32_t>(handles.size()));
  validation = validateImportedWireChunk(wire);
  checkEq(static_cast<int>(validation.status),
          static_cast<int>(ImportedWireChunkValidationStatus::InvalidRecordRange),
          std::string(name) + " rejects handle range past table");

  records[0] = wireRecordHeader(type, 0u,
                                static_cast<std::uint32_t>(payload.size()),
                                0u, 1u);
  auto badHandles = handles;
  badHandles[0].kind = D9C_CHUNK_HANDLE_KIND_VERTEX_DECL + 1u;
  wire = makeImportedWireChunkView(
      records.data(), static_cast<std::uint32_t>(records.size()),
      payload.data(), static_cast<std::uint32_t>(payload.size()),
      badHandles.data(), static_cast<std::uint32_t>(badHandles.size()));
  validation = validateImportedWireChunk(wire);
  checkEq(static_cast<int>(validation.status),
          static_cast<int>(ImportedWireChunkValidationStatus::InvalidHandleEntry),
          std::string(name) + " rejects invalid wire handle kind");

  badHandles = handles;
  badHandles[0].reserved1 = 1u;
  wire = makeImportedWireChunkView(
      records.data(), static_cast<std::uint32_t>(records.size()),
      payload.data(), static_cast<std::uint32_t>(payload.size()),
      badHandles.data(), static_cast<std::uint32_t>(badHandles.size()));
  validation = validateImportedWireChunk(wire);
  checkEq(static_cast<int>(validation.status),
          static_cast<int>(ImportedWireChunkValidationStatus::InvalidHandleEntry),
          std::string(name) + " rejects wire handle reserved fields");

  badHandles = handles;
  badHandles[0].generation = 1u;
  wire = makeImportedWireChunkView(
      records.data(), static_cast<std::uint32_t>(records.size()),
      payload.data(), static_cast<std::uint32_t>(payload.size()),
      badHandles.data(), static_cast<std::uint32_t>(badHandles.size()));
  validation = validateImportedWireChunk(wire);
  checkEq(static_cast<int>(validation.status),
          static_cast<int>(ImportedWireChunkValidationStatus::InvalidHandleEntry),
          std::string(name) + " rejects wire handle generations");
}

void checkWireAcceptsRecordBytes(const std::vector<std::uint8_t>& payload,
                                 std::uint32_t type,
                                 std::string_view name) {
  std::vector<D9CCommandChunkWireRecordHeader> records{
      wireRecordHeader(type, 0u, static_cast<std::uint32_t>(payload.size())),
  };
  const auto wire = makeImportedWireChunkView(
      records.data(), static_cast<std::uint32_t>(records.size()),
      payload.data(), static_cast<std::uint32_t>(payload.size()), nullptr, 0u);
  const auto validation = validateImportedWireChunk(wire);
  check(validation.valid(), std::string(name) + " validates as DOD wire record");
  checkEq(validation.parsedRecordCount, 1u,
          std::string(name) + " wire parse count");

  const auto record = nextImportedRecord(wire, 0u);
  check(record.has_value(), std::string(name) + " imports from wire view");
  check(record->valid(), std::string(name) + " imported record validates");
  checkEq(record->header.type, type, std::string(name) + " imported type");
  checkEq(record->header.size, static_cast<std::uint32_t>(payload.size()),
          std::string(name) + " imported size");
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

void testAllCommandIdsValidateWithExpectedRecordShapes() {
  const auto draw = makeDrawPrimitiveRecord(0u, 1u);
  checkValidRecordBytes(recordBytes(draw), D9C_COMMAND_RECORD_DRAW_PRIMITIVE,
                        sizeof(draw), sizeof(draw),
                        "DRAW_PRIMITIVE record");

  const auto indexed = makeDrawIndexedPrimitiveRecord(0u, 1u);
  checkValidRecordBytes(recordBytes(indexed),
                        D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE,
                        sizeof(indexed), sizeof(indexed),
                        "DRAW_INDEXED_PRIMITIVE record");

  const auto drawUp = makeDrawPrimitiveUPRecord(64u);
  checkValidRecordBytes(declaredRecordBytes(drawUp),
                        D9C_COMMAND_RECORD_DRAW_PRIMITIVE_UP,
                        sizeof(D9CCommandRecordDrawPrimitiveUP),
                        drawUp.header.size,
                        "DRAW_PRIMITIVE_UP record");

  const auto indexedUp = makeDrawIndexedPrimitiveUPRecord(6u, 64u);
  checkValidRecordBytes(declaredRecordBytes(indexedUp),
                        D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE_UP,
                        sizeof(D9CCommandRecordDrawIndexedPrimitiveUP),
                        indexedUp.header.size,
                        "DRAW_INDEXED_PRIMITIVE_UP record");

  const auto vsFloat = makeSetConstRecord(
      D9C_COMMAND_RECORD_SET_VS_CONST_F, 2u, sizeof(float) * 4u);
  checkValidRecordBytes(declaredRecordBytes(vsFloat),
                        D9C_COMMAND_RECORD_SET_VS_CONST_F,
                        sizeof(D9CCommandRecordSetConst),
                        vsFloat.header.size,
                        "SET_VS_CONST_F record");

  const auto vsInt = makeSetConstRecord(
      D9C_COMMAND_RECORD_SET_VS_CONST_I, 2u, sizeof(std::int32_t) * 4u);
  checkValidRecordBytes(declaredRecordBytes(vsInt),
                        D9C_COMMAND_RECORD_SET_VS_CONST_I,
                        sizeof(D9CCommandRecordSetConst),
                        vsInt.header.size,
                        "SET_VS_CONST_I record");

  const auto vsBool = makeSetConstRecord(
      D9C_COMMAND_RECORD_SET_VS_CONST_B, 3u, sizeof(std::uint32_t));
  checkValidRecordBytes(declaredRecordBytes(vsBool),
                        D9C_COMMAND_RECORD_SET_VS_CONST_B,
                        sizeof(D9CCommandRecordSetConst),
                        vsBool.header.size,
                        "SET_VS_CONST_B record");

  const auto psFloat = makeSetConstRecord(
      D9C_COMMAND_RECORD_SET_PS_CONST_F, 2u, sizeof(float) * 4u);
  checkValidRecordBytes(declaredRecordBytes(psFloat),
                        D9C_COMMAND_RECORD_SET_PS_CONST_F,
                        sizeof(D9CCommandRecordSetConst),
                        psFloat.header.size,
                        "SET_PS_CONST_F record");

  const auto psInt = makeSetConstRecord(
      D9C_COMMAND_RECORD_SET_PS_CONST_I, 2u, sizeof(std::int32_t) * 4u);
  checkValidRecordBytes(declaredRecordBytes(psInt),
                        D9C_COMMAND_RECORD_SET_PS_CONST_I,
                        sizeof(D9CCommandRecordSetConst),
                        psInt.header.size,
                        "SET_PS_CONST_I record");

  const auto psBool = makeSetConstRecord(
      D9C_COMMAND_RECORD_SET_PS_CONST_B, 3u, sizeof(std::uint32_t));
  checkValidRecordBytes(declaredRecordBytes(psBool),
                        D9C_COMMAND_RECORD_SET_PS_CONST_B,
                        sizeof(D9CCommandRecordSetConst),
                        psBool.header.size,
                        "SET_PS_CONST_B record");

  const auto clear = makeClearRecord(2u);
  checkValidRecordBytes(declaredRecordBytes(clear), D9C_COMMAND_RECORD_CLEAR,
                        sizeof(D9CCommandRecordClear), clear.header.size,
                        "CLEAR record");

  const auto present = makePresentRecord();
  checkValidRecordBytes(recordBytes(present), D9C_COMMAND_RECORD_PRESENT,
                        sizeof(present), sizeof(present),
                        "PRESENT record");

  const auto stretch = makeStretchRectRecord();
  checkValidRecordBytes(recordBytes(stretch), D9C_COMMAND_RECORD_STRETCH_RECT,
                        sizeof(stretch), sizeof(stretch),
                        "STRETCH_RECT record");

  const auto color = makeColorFillRecord();
  checkValidRecordBytes(recordBytes(color), D9C_COMMAND_RECORD_COLOR_FILL,
                        sizeof(color), sizeof(color),
                        "COLOR_FILL record");

  const auto updateTexture = makeUpdateTextureRecord();
  checkValidRecordBytes(recordBytes(updateTexture),
                        D9C_COMMAND_RECORD_UPDATE_TEXTURE,
                        sizeof(updateTexture), sizeof(updateTexture),
                        "UPDATE_TEXTURE record");

  const auto updateSurface = makeUpdateSurfaceRecord();
  checkValidRecordBytes(recordBytes(updateSurface),
                        D9C_COMMAND_RECORD_UPDATE_SURFACE,
                        sizeof(updateSurface), sizeof(updateSurface),
                        "UPDATE_SURFACE record");

  const auto query = makeQueryIssueRecord();
  checkValidRecordBytes(recordBytes(query), D9C_COMMAND_RECORD_QUERY_ISSUE,
                        sizeof(query), sizeof(query),
                        "QUERY_ISSUE record");

  const auto readback = makeReadbackRecord();
  checkValidRecordBytes(recordBytes(readback), D9C_COMMAND_RECORD_READBACK,
                        sizeof(readback), sizeof(readback),
                        "READBACK record");

  const auto apply = makeApplyStateRecord();
  checkValidRecordBytes(recordBytes(apply), D9C_COMMAND_RECORD_APPLY_STATE,
                        sizeof(apply), sizeof(apply),
                        "APPLY_STATE record");
}

void testDrawUpValidationMatrix() {
  auto drawUp = makeDrawPrimitiveUPRecord(64u);
  checkValidRecordBytes(declaredRecordBytes(drawUp),
                        D9C_COMMAND_RECORD_DRAW_PRIMITIVE_UP,
                        sizeof(D9CCommandRecordDrawPrimitiveUP),
                        drawUp.header.size,
                        "draw primitive UP with vertex tail");

  drawUp = makeDrawPrimitiveUPRecord(0u);
  checkValidRecordBytes(recordBytes(drawUp),
                        D9C_COMMAND_RECORD_DRAW_PRIMITIVE_UP,
                        sizeof(D9CCommandRecordDrawPrimitiveUP),
                        sizeof(D9CCommandRecordDrawPrimitiveUP),
                        "draw primitive UP minimum record");

  auto tooSmallDrawUp = drawUp;
  tooSmallDrawUp.header.size =
      static_cast<std::uint32_t>(sizeof(D9CCommandRecordDrawPrimitiveUP) - 1u);
  checkStatus(recordBytes(tooSmallDrawUp, tooSmallDrawUp.header.size),
              D9CCommandRecordValidationStatus::SizeTooSmall,
              "draw primitive UP rejects undersized fixed header");

  auto truncatedDrawUp = makeDrawPrimitiveUPRecord(64u);
  checkStatus(recordBytes(truncatedDrawUp, truncatedDrawUp.header.size - 1u),
              D9CCommandRecordValidationStatus::TruncatedRecord,
              "draw primitive UP rejects unavailable declared tail");

  auto indexedUp = makeDrawIndexedPrimitiveUPRecord(6u, 64u);
  checkValidRecordBytes(declaredRecordBytes(indexedUp),
                        D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE_UP,
                        sizeof(D9CCommandRecordDrawIndexedPrimitiveUP),
                        indexedUp.header.size,
                        "draw indexed primitive UP with index and vertex tails");

  indexedUp = makeDrawIndexedPrimitiveUPRecord(0u, 0u);
  checkValidRecordBytes(recordBytes(indexedUp),
                        D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE_UP,
                        sizeof(D9CCommandRecordDrawIndexedPrimitiveUP),
                        sizeof(D9CCommandRecordDrawIndexedPrimitiveUP),
                        "draw indexed primitive UP minimum record");

  auto tooSmallIndexedUp = indexedUp;
  tooSmallIndexedUp.header.size = static_cast<std::uint32_t>(
      sizeof(D9CCommandRecordDrawIndexedPrimitiveUP) - 1u);
  checkStatus(recordBytes(tooSmallIndexedUp, tooSmallIndexedUp.header.size),
              D9CCommandRecordValidationStatus::SizeTooSmall,
              "draw indexed primitive UP rejects undersized fixed header");

  auto truncatedIndexedUp = makeDrawIndexedPrimitiveUPRecord(6u, 64u);
  checkStatus(recordBytes(truncatedIndexedUp, truncatedIndexedUp.header.size - 1u),
              D9CCommandRecordValidationStatus::TruncatedRecord,
              "draw indexed primitive UP rejects unavailable declared tail");
}

void testSurfaceQueryRecordValidationMatrix() {
  checkExactRecordSizeMatrix(makeQueryIssueRecord(), "QUERY_ISSUE");
  checkExactRecordSizeMatrix(makeColorFillRecord(), "COLOR_FILL");
  checkExactRecordSizeMatrix(makeUpdateSurfaceRecord(), "UPDATE_SURFACE");
  checkExactRecordSizeMatrix(makeUpdateTextureRecord(), "UPDATE_TEXTURE");
  checkExactRecordSizeMatrix(makeStretchRectRecord(), "STRETCH_RECT");
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

void testCommandChunkWireBlobRejectsLegacyRawRecordStream() {
  const auto present = makePresentRecord();
  const auto draw = makeDrawPrimitiveRecord(4u, 2u);

  std::vector<std::uint8_t> bytes;
  appendRecord(bytes, present);
  appendRecord(bytes, draw);

  const auto decoded = makeImportedWireChunkBlobView(
      bytes.data(), static_cast<std::uint32_t>(bytes.size()));
  check(!decoded.valid(), "raw record stream is not accepted as a wire blob");
  check(!decoded.wireHeaderCandidate(),
        "raw record stream does not pass DOD wire header identity checks");
  checkEq(static_cast<int>(decoded.status),
          static_cast<int>(ImportedWireChunkValidationStatus::InvalidChunkHeader),
          "raw record stream fails as an invalid chunk header");

  const auto empty = makeImportedWireChunkBlobView(nullptr, 0u);
  checkEq(static_cast<int>(empty.status),
          static_cast<int>(ImportedWireChunkValidationStatus::MissingChunkHeader),
          "empty command chunk blob is rejected before import");
}

void testImportedWireChunkRejectsMalformedPayloadRange() {
  const auto present = makePresentRecord();
  const auto draw = makeDrawPrimitiveRecord(4u, 2u);
  std::vector<std::uint8_t> bytes;
  appendRecord(bytes, present);
  appendRecord(bytes, draw);

  const std::vector<D9CCommandChunkWireRecordHeader> validRecords{
      wireRecordHeader(D9C_COMMAND_RECORD_PRESENT, 0u,
                       static_cast<std::uint32_t>(sizeof(present))),
      wireRecordHeader(D9C_COMMAND_RECORD_DRAW_PRIMITIVE,
                       static_cast<std::uint32_t>(sizeof(present)),
                       static_cast<std::uint32_t>(sizeof(draw))),
  };
  std::vector<D9CCommandChunkWireRecordHeader> records = validRecords;
  records[0].payloadOffset = static_cast<std::uint32_t>(bytes.size());
  records[0].payloadSize = 1u;

  auto wire = makeImportedWireChunkView(
      records.data(), static_cast<std::uint32_t>(records.size()), bytes.data(),
      static_cast<std::uint32_t>(bytes.size()), nullptr, 0u);
  const auto validation = validateImportedWireChunk(wire);
  checkEq(static_cast<int>(validation.status),
          static_cast<int>(ImportedWireChunkValidationStatus::InvalidRecordRange),
          "wire payload range past arena is rejected");
  checkEq(validation.failedRecordIndex, 0u,
          "wire payload range reports failed record index");

  records = validRecords;
  records[0].payloadSize = static_cast<std::uint32_t>(bytes.size());
  wire = makeImportedWireChunkView(
      records.data(), static_cast<std::uint32_t>(records.size()), bytes.data(),
      static_cast<std::uint32_t>(bytes.size()), nullptr, 0u);
  const auto oversizedValidation = validateImportedWireChunk(wire);
  checkEq(static_cast<int>(oversizedValidation.status),
          static_cast<int>(ImportedWireChunkValidationStatus::InvalidRecord),
          "wire payload range spanning extra record bytes is rejected");
  checkEq(static_cast<int>(oversizedValidation.failedRecord.validation.status),
          static_cast<int>(D9CCommandRecordValidationStatus::SizeMismatch),
          "wire oversized payload preserves legacy record validation status");
}

void testImportedWireChunkEnforcesHandleTableAndRanges() {
  const auto present = makePresentRecord();
  auto bytes = recordBytes(present);
  std::vector<D9CCommandChunkWireHandleEntry> handles{
      wireHandleEntry(D9C_CHUNK_HANDLE_KIND_TEXTURE, 0x1000u),
      wireHandleEntry(D9C_CHUNK_HANDLE_KIND_BUFFER, 0x2000u),
  };
  const std::vector<D9CCommandChunkWireRecordHeader> validRecords{
      wireRecordHeader(D9C_COMMAND_RECORD_PRESENT, 0u,
                       static_cast<std::uint32_t>(sizeof(present)), 0u,
                       static_cast<std::uint32_t>(handles.size())),
  };

  auto records = validRecords;
  records[0].firstHandle = 2u;
  records[0].handleCount = 1u;
  auto wire = makeImportedWireChunkView(
      records.data(), static_cast<std::uint32_t>(records.size()), bytes.data(),
      static_cast<std::uint32_t>(bytes.size()), handles.data(),
      static_cast<std::uint32_t>(handles.size()));
  auto validation = validateImportedWireChunk(wire);
  checkEq(static_cast<int>(validation.status),
          static_cast<int>(ImportedWireChunkValidationStatus::InvalidRecordRange),
          "wire handle range past table is rejected");
  checkEq(validation.failedRecordIndex, 0u,
          "wire handle range reports failed record index");

  records = validRecords;
  records[0].flags = 1u;
  wire = makeImportedWireChunkView(
      records.data(), static_cast<std::uint32_t>(records.size()), bytes.data(),
      static_cast<std::uint32_t>(bytes.size()), handles.data(),
      static_cast<std::uint32_t>(handles.size()));
  validation = validateImportedWireChunk(wire);
  checkEq(static_cast<int>(validation.status),
          static_cast<int>(ImportedWireChunkValidationStatus::InvalidRecordHeader),
          "wire record header rejects unsupported flags");

  records[0].flags = D9C_COMMAND_CHUNK_WIRE_RECORD_FLAG_NONE;
  records[0].reserved1 = 1u;
  wire = makeImportedWireChunkView(
      records.data(), static_cast<std::uint32_t>(records.size()), bytes.data(),
      static_cast<std::uint32_t>(bytes.size()), handles.data(),
      static_cast<std::uint32_t>(handles.size()));
  validation = validateImportedWireChunk(wire);
  checkEq(static_cast<int>(validation.status),
          static_cast<int>(ImportedWireChunkValidationStatus::InvalidRecordHeader),
          "wire record header rejects nonzero reserved fields");

  records = validRecords;
  handles[1].kind = D9C_CHUNK_HANDLE_KIND_VERTEX_DECL + 1u;
  wire = makeImportedWireChunkView(
      records.data(), static_cast<std::uint32_t>(records.size()), bytes.data(),
      static_cast<std::uint32_t>(bytes.size()), handles.data(),
      static_cast<std::uint32_t>(handles.size()));
  validation = validateImportedWireChunk(wire);
  checkEq(static_cast<int>(validation.status),
          static_cast<int>(ImportedWireChunkValidationStatus::InvalidHandleEntry),
          "wire handle table rejects unknown handle kind");
  checkEq(validation.failedHandleIndex, 1u,
          "wire handle table reports failed handle index");

  handles[1].kind = D9C_CHUNK_HANDLE_KIND_BUFFER;
  handles[1].reserved0 = 1u;
  wire = makeImportedWireChunkView(
      records.data(), static_cast<std::uint32_t>(records.size()), bytes.data(),
      static_cast<std::uint32_t>(bytes.size()), handles.data(),
      static_cast<std::uint32_t>(handles.size()));
  validation = validateImportedWireChunk(wire);
  checkEq(static_cast<int>(validation.status),
          static_cast<int>(ImportedWireChunkValidationStatus::InvalidHandleEntry),
          "wire handle table rejects nonzero reserved fields");

  handles[1].reserved0 = 0u;
  handles[1].reserved1 = 1u;
  wire = makeImportedWireChunkView(
      records.data(), static_cast<std::uint32_t>(records.size()), bytes.data(),
      static_cast<std::uint32_t>(bytes.size()), handles.data(),
      static_cast<std::uint32_t>(handles.size()));
  validation = validateImportedWireChunk(wire);
  checkEq(static_cast<int>(validation.status),
          static_cast<int>(ImportedWireChunkValidationStatus::InvalidHandleEntry),
          "wire handle table rejects nonzero trailing reserved fields");

  handles[1].reserved1 = 0u;
  handles[1].generation = 1u;
  wire = makeImportedWireChunkView(
      records.data(), static_cast<std::uint32_t>(records.size()), bytes.data(),
      static_cast<std::uint32_t>(bytes.size()), handles.data(),
      static_cast<std::uint32_t>(handles.size()));
  validation = validateImportedWireChunk(wire);
  checkEq(static_cast<int>(validation.status),
          static_cast<int>(ImportedWireChunkValidationStatus::InvalidHandleEntry),
          "wire handle table rejects unsupported handle generations");
}

void testImportedWireRecordHandleRangesSelectSubsets() {
  const auto present = makePresentRecord();
  const auto draw = makeDrawPrimitiveRecord(4u, 2u);

  std::vector<std::uint8_t> arena;
  appendRecord(arena, present);
  appendRecord(arena, draw);

  std::vector<D9CCommandChunkWireHandleEntry> handles{
      D9CCommandChunkWireHandleEntry{
          .kind = D9C_CHUNK_HANDLE_KIND_TEXTURE,
          .generation = D9C_COMMAND_CHUNK_WIRE_HANDLE_GENERATION_NONE,
          .opaqueHandle = 0x1000u,
          .reserved0 = 0u,
          .reserved1 = 0u,
      },
      D9CCommandChunkWireHandleEntry{
          .kind = D9C_CHUNK_HANDLE_KIND_BUFFER,
          .generation = D9C_COMMAND_CHUNK_WIRE_HANDLE_GENERATION_NONE,
          .opaqueHandle = 0x2000u,
          .reserved0 = 0u,
          .reserved1 = 0u,
      },
      D9CCommandChunkWireHandleEntry{
          .kind = D9C_CHUNK_HANDLE_KIND_SURFACE,
          .generation = D9C_COMMAND_CHUNK_WIRE_HANDLE_GENERATION_NONE,
          .opaqueHandle = 0x3000u,
          .reserved0 = 0u,
          .reserved1 = 0u,
      },
  };

  std::vector<D9CCommandChunkWireRecordHeader> records{
      D9CCommandChunkWireRecordHeader{
          .type = static_cast<std::uint32_t>(D9C_COMMAND_RECORD_PRESENT),
          .flags = D9C_COMMAND_CHUNK_WIRE_RECORD_FLAG_NONE,
          .payloadOffset = 0u,
          .payloadSize = static_cast<std::uint32_t>(sizeof(present)),
          .firstHandle = 1u,
          .handleCount = 1u,
          .reserved0 = 0u,
          .reserved1 = 0u,
      },
      D9CCommandChunkWireRecordHeader{
          .type = static_cast<std::uint32_t>(D9C_COMMAND_RECORD_DRAW_PRIMITIVE),
          .flags = D9C_COMMAND_CHUNK_WIRE_RECORD_FLAG_NONE,
          .payloadOffset = static_cast<std::uint32_t>(sizeof(present)),
          .payloadSize = static_cast<std::uint32_t>(sizeof(draw)),
          .firstHandle = 2u,
          .handleCount = 1u,
          .reserved0 = 0u,
          .reserved1 = 0u,
      },
  };

  const auto wire = makeImportedWireChunkView(
      records.data(), static_cast<std::uint32_t>(records.size()), arena.data(),
      static_cast<std::uint32_t>(arena.size()), handles.data(),
      static_cast<std::uint32_t>(handles.size()));
  check(validateImportedWireChunk(wire).valid(),
        "wire chunk with per-record handle subsets validates");

  const auto firstRange = makeImportedWireRecordHandleView(wire, 0u);
  check(firstRange.handles == handles.data() + 1u,
        "first wire record handle range starts at selected table entry");
  checkEq(firstRange.handleCount, 1u,
          "first wire record handle range exposes selected count");

  ImportedChunkHandleSet firstHandles;
  check(collectImportedWireRecordHandles(wire, 0u, firstHandles),
        "first wire record handle subset collects");
  const auto firstEntries = makeImportedChunkHandleEntries(firstHandles);
  checkEq(firstEntries.size(), static_cast<std::size_t>(1),
          "first wire record collects only its handle range");
  check(containsHandle(firstEntries, D9C_CHUNK_HANDLE_KIND_BUFFER, 0x2000u),
        "first wire record selected the buffer handle");

  ImportedChunkHandleSet secondHandles;
  check(collectImportedWireRecordHandles(wire, 1u, secondHandles),
        "second wire record handle subset collects");
  const auto secondEntries = makeImportedChunkHandleEntries(secondHandles);
  checkEq(secondEntries.size(), static_cast<std::size_t>(1),
          "second wire record collects only its handle range");
  check(containsHandle(secondEntries, D9C_CHUNK_HANDLE_KIND_SURFACE, 0x3000u),
        "second wire record selected the surface handle");

  ImportedChunkHandleSet chunkHandles;
  check(collectImportedWireChunkHandles(wire, chunkHandles),
        "wire chunk handle collection walks per-record ranges");
  const auto chunkEntries = makeImportedChunkHandleEntries(chunkHandles);
  checkEq(chunkEntries.size(), static_cast<std::size_t>(2),
          "wire chunk collection ignores unreferenced table entries");
  check(!containsHandle(chunkEntries, D9C_CHUNK_HANDLE_KIND_TEXTURE, 0x1000u),
        "wire chunk collection excludes handles outside all record ranges");
}

void testImportedWireIterationBuildsLegacyRecordViews() {
  const auto present = makePresentRecord();
  const auto draw = makeDrawPrimitiveRecord(4u, 2u);

  std::vector<std::uint8_t> bytes;
  appendRecord(bytes, present);
  appendRecord(bytes, draw);

  std::vector<D9CCommandChunkWireRecordHeader> records{
      wireRecordHeader(D9C_COMMAND_RECORD_PRESENT, 0u,
                       static_cast<std::uint32_t>(sizeof(present))),
      wireRecordHeader(D9C_COMMAND_RECORD_DRAW_PRIMITIVE,
                       static_cast<std::uint32_t>(sizeof(present)),
                       static_cast<std::uint32_t>(sizeof(draw))),
  };
  const auto wire = makeImportedWireChunkView(
      records.data(), static_cast<std::uint32_t>(records.size()), bytes.data(),
      static_cast<std::uint32_t>(bytes.size()), nullptr, 0u);

  const auto first = nextImportedRecord(wire, 0u);
  check(first.has_value(), "first wire imported record exists");
  check(first->valid(), "first wire imported record validates as legacy record");
  check(first->record == bytes.data(),
        "first wire imported record points at legacy payload arena");
  checkEq(first->offset, 0u, "first wire imported record offset");
  checkEq(first->index, 0u, "first wire imported record index");
  checkEq(first->header.type,
          static_cast<std::uint32_t>(D9C_COMMAND_RECORD_PRESENT),
          "first wire imported record type");
  checkEq(first->header.size, static_cast<std::uint32_t>(sizeof(present)),
          "first wire imported record size");

  const auto second = nextImportedRecord(wire, first->nextIndex());
  check(second.has_value(), "second wire imported record exists");
  check(second->valid(), "second wire imported record validates as legacy record");
  check(second->record == bytes.data() + sizeof(present),
        "second wire imported record points at legacy payload arena");
  checkEq(second->offset, static_cast<std::uint32_t>(sizeof(present)),
          "second wire imported record offset");
  checkEq(second->index, 1u, "second wire imported record index");
  checkEq(second->header.type,
          static_cast<std::uint32_t>(D9C_COMMAND_RECORD_DRAW_PRIMITIVE),
          "second wire imported record type");

  const auto end = nextImportedRecord(wire, second->nextIndex());
  check(!end.has_value(), "wire imported record iteration stops at record count");
}

void testImportedWireAcceptsAllCommandIds() {
  const auto draw = makeDrawPrimitiveRecord(0u, 1u);
  checkWireAcceptsRecordBytes(recordBytes(draw),
                              D9C_COMMAND_RECORD_DRAW_PRIMITIVE,
                              "DRAW_PRIMITIVE");

  const auto indexed = makeDrawIndexedPrimitiveRecord(0u, 1u);
  checkWireAcceptsRecordBytes(recordBytes(indexed),
                              D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE,
                              "DRAW_INDEXED_PRIMITIVE");

  const auto drawUp = makeDrawPrimitiveUPRecord(64u);
  checkWireAcceptsRecordBytes(declaredRecordBytes(drawUp),
                              D9C_COMMAND_RECORD_DRAW_PRIMITIVE_UP,
                              "DRAW_PRIMITIVE_UP");

  const auto indexedUp = makeDrawIndexedPrimitiveUPRecord(6u, 64u);
  checkWireAcceptsRecordBytes(declaredRecordBytes(indexedUp),
                              D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE_UP,
                              "DRAW_INDEXED_PRIMITIVE_UP");

  const auto vsFloat = makeSetConstRecord(
      D9C_COMMAND_RECORD_SET_VS_CONST_F, 2u, sizeof(float) * 4u);
  checkWireAcceptsRecordBytes(declaredRecordBytes(vsFloat),
                              D9C_COMMAND_RECORD_SET_VS_CONST_F,
                              "SET_VS_CONST_F");

  const auto vsInt = makeSetConstRecord(
      D9C_COMMAND_RECORD_SET_VS_CONST_I, 2u, sizeof(std::int32_t) * 4u);
  checkWireAcceptsRecordBytes(declaredRecordBytes(vsInt),
                              D9C_COMMAND_RECORD_SET_VS_CONST_I,
                              "SET_VS_CONST_I");

  const auto vsBool = makeSetConstRecord(
      D9C_COMMAND_RECORD_SET_VS_CONST_B, 3u, sizeof(std::uint32_t));
  checkWireAcceptsRecordBytes(declaredRecordBytes(vsBool),
                              D9C_COMMAND_RECORD_SET_VS_CONST_B,
                              "SET_VS_CONST_B");

  const auto psFloat = makeSetConstRecord(
      D9C_COMMAND_RECORD_SET_PS_CONST_F, 2u, sizeof(float) * 4u);
  checkWireAcceptsRecordBytes(declaredRecordBytes(psFloat),
                              D9C_COMMAND_RECORD_SET_PS_CONST_F,
                              "SET_PS_CONST_F");

  const auto psInt = makeSetConstRecord(
      D9C_COMMAND_RECORD_SET_PS_CONST_I, 2u, sizeof(std::int32_t) * 4u);
  checkWireAcceptsRecordBytes(declaredRecordBytes(psInt),
                              D9C_COMMAND_RECORD_SET_PS_CONST_I,
                              "SET_PS_CONST_I");

  const auto psBool = makeSetConstRecord(
      D9C_COMMAND_RECORD_SET_PS_CONST_B, 3u, sizeof(std::uint32_t));
  checkWireAcceptsRecordBytes(declaredRecordBytes(psBool),
                              D9C_COMMAND_RECORD_SET_PS_CONST_B,
                              "SET_PS_CONST_B");

  const auto clear = makeClearRecord(2u);
  checkWireAcceptsRecordBytes(declaredRecordBytes(clear), D9C_COMMAND_RECORD_CLEAR,
                              "CLEAR");

  const auto present = makePresentRecord();
  checkWireAcceptsRecordBytes(recordBytes(present), D9C_COMMAND_RECORD_PRESENT,
                              "PRESENT");

  const auto stretch = makeStretchRectRecord();
  checkWireAcceptsRecordBytes(recordBytes(stretch),
                              D9C_COMMAND_RECORD_STRETCH_RECT,
                              "STRETCH_RECT");

  const auto color = makeColorFillRecord();
  checkWireAcceptsRecordBytes(recordBytes(color), D9C_COMMAND_RECORD_COLOR_FILL,
                              "COLOR_FILL");

  const auto updateTexture = makeUpdateTextureRecord();
  checkWireAcceptsRecordBytes(recordBytes(updateTexture),
                              D9C_COMMAND_RECORD_UPDATE_TEXTURE,
                              "UPDATE_TEXTURE");

  const auto updateSurface = makeUpdateSurfaceRecord();
  checkWireAcceptsRecordBytes(recordBytes(updateSurface),
                              D9C_COMMAND_RECORD_UPDATE_SURFACE,
                              "UPDATE_SURFACE");

  const auto query = makeQueryIssueRecord();
  checkWireAcceptsRecordBytes(recordBytes(query), D9C_COMMAND_RECORD_QUERY_ISSUE,
                              "QUERY_ISSUE");

  const auto readback = makeReadbackRecord();
  checkWireAcceptsRecordBytes(recordBytes(readback), D9C_COMMAND_RECORD_READBACK,
                              "READBACK");

  const auto apply = makeApplyStateRecord();
  checkWireAcceptsRecordBytes(recordBytes(apply), D9C_COMMAND_RECORD_APPLY_STATE,
                              "APPLY_STATE");
}

void testImportedWireSpecialRecordValidationMatrix() {
  const auto drawUp = makeDrawPrimitiveUPRecord(64u);
  checkWireRecordMatrix(declaredRecordBytes(drawUp),
                        D9C_COMMAND_RECORD_DRAW_PRIMITIVE_UP,
                        "DRAW_PRIMITIVE_UP");

  const auto indexedUp = makeDrawIndexedPrimitiveUPRecord(6u, 64u);
  checkWireRecordMatrix(declaredRecordBytes(indexedUp),
                        D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE_UP,
                        "DRAW_INDEXED_PRIMITIVE_UP");

  const auto query = makeQueryIssueRecord();
  checkWireRecordMatrix(recordBytes(query), D9C_COMMAND_RECORD_QUERY_ISSUE,
                        "QUERY_ISSUE");

  const auto color = makeColorFillRecord();
  checkWireRecordMatrix(recordBytes(color), D9C_COMMAND_RECORD_COLOR_FILL,
                        "COLOR_FILL");

  const auto updateSurface = makeUpdateSurfaceRecord();
  checkWireRecordMatrix(recordBytes(updateSurface),
                        D9C_COMMAND_RECORD_UPDATE_SURFACE,
                        "UPDATE_SURFACE");

  const auto updateTexture = makeUpdateTextureRecord();
  checkWireRecordMatrix(recordBytes(updateTexture),
                        D9C_COMMAND_RECORD_UPDATE_TEXTURE,
                        "UPDATE_TEXTURE");

  const auto stretch = makeStretchRectRecord();
  checkWireRecordMatrix(recordBytes(stretch),
                        D9C_COMMAND_RECORD_STRETCH_RECT,
                        "STRETCH_RECT");
}

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

void testImportedWireBlobViewUsesHeaderTablesAndArena() {
  const auto present = makePresentRecord();
  const auto draw = makeDrawPrimitiveRecord(11u, 2u);

  constexpr std::uint32_t drawPayloadOffset = 8u;
  const auto presentPayloadOffset =
      static_cast<std::uint32_t>(drawPayloadOffset + sizeof(draw) + 32u);
  std::vector<std::uint8_t> arena(presentPayloadOffset + sizeof(present));
  writeObject(arena, drawPayloadOffset, draw);
  writeObject(arena, presentPayloadOffset, present);

  std::vector<D9CCommandChunkWireRecordHeader> records{
      D9CCommandChunkWireRecordHeader{
          .type = static_cast<std::uint32_t>(D9C_COMMAND_RECORD_PRESENT),
          .flags = D9C_COMMAND_CHUNK_WIRE_RECORD_FLAG_NONE,
          .payloadOffset = presentPayloadOffset,
          .payloadSize = static_cast<std::uint32_t>(sizeof(present)),
          .firstHandle = 1u,
          .handleCount = 1u,
          .reserved0 = 0u,
          .reserved1 = 0u,
      },
      D9CCommandChunkWireRecordHeader{
          .type = static_cast<std::uint32_t>(D9C_COMMAND_RECORD_DRAW_PRIMITIVE),
          .flags = D9C_COMMAND_CHUNK_WIRE_RECORD_FLAG_NONE,
          .payloadOffset = drawPayloadOffset,
          .payloadSize = static_cast<std::uint32_t>(sizeof(draw)),
          .firstHandle = 0u,
          .handleCount = 1u,
          .reserved0 = 0u,
          .reserved1 = 0u,
      },
  };
  std::vector<D9CCommandChunkWireHandleEntry> handles{
      D9CCommandChunkWireHandleEntry{
          .kind = D9C_CHUNK_HANDLE_KIND_TEXTURE,
          .generation = D9C_COMMAND_CHUNK_WIRE_HANDLE_GENERATION_NONE,
          .opaqueHandle = 0x1000u,
          .reserved0 = 0u,
          .reserved1 = 0u,
      },
      D9CCommandChunkWireHandleEntry{
          .kind = D9C_CHUNK_HANDLE_KIND_SURFACE,
          .generation = D9C_COMMAND_CHUNK_WIRE_HANDLE_GENERATION_NONE,
          .opaqueHandle = 0x3000u,
          .reserved0 = 0u,
          .reserved1 = 0u,
      },
  };

  const auto recordTableOffset =
      static_cast<std::uint32_t>(sizeof(D9CCommandChunkWireHeader));
  const auto recordTableBytes =
      static_cast<std::uint32_t>(records.size() * sizeof(records[0]));
  const auto handleTableOffset = recordTableOffset + recordTableBytes;
  const auto handleTableBytes =
      static_cast<std::uint32_t>(handles.size() * sizeof(handles[0]));
  const auto payloadArenaOffset = handleTableOffset + handleTableBytes + 16u;

  D9CCommandChunkWireHeader header{};
  header.version = D9C_COMMAND_CHUNK_WIRE_VERSION;
  header.headerSize = D9C_COMMAND_CHUNK_WIRE_HEADER_SIZE;
  header.recordHeaderSize = D9C_COMMAND_CHUNK_WIRE_RECORD_HEADER_SIZE;
  header.handleEntrySize = D9C_COMMAND_CHUNK_WIRE_HANDLE_ENTRY_SIZE;
  header.recordTableOffset = recordTableOffset;
  header.recordCount = static_cast<std::uint32_t>(records.size());
  header.handleTableOffset = handleTableOffset;
  header.handleCount = static_cast<std::uint32_t>(handles.size());
  header.payloadArenaOffset = payloadArenaOffset;
  header.payloadArenaSize = static_cast<std::uint32_t>(arena.size());

  std::vector<std::uint8_t> blob(payloadArenaOffset + arena.size());
  writeObject(blob, 0u, header);
  std::memcpy(blob.data() + recordTableOffset, records.data(), recordTableBytes);
  std::memcpy(blob.data() + handleTableOffset, handles.data(), handleTableBytes);
  std::memcpy(blob.data() + payloadArenaOffset, arena.data(), arena.size());

  const auto decoded = makeImportedWireChunkBlobView(
      blob.data(), static_cast<std::uint32_t>(blob.size()));
  check(decoded.wireHeaderCandidate(), "wire blob decoder recognizes DOD header");
  check(decoded.valid(), "wire blob decoder validates header tables and arena");
  checkEq(decoded.chunk.recordCount, 2u, "wire blob decoder exposes record table count");
  checkEq(decoded.chunk.handleCount, 2u, "wire blob decoder exposes handle table count");

  const auto first = nextImportedRecord(decoded.chunk, 0u);
  check(first.has_value(), "wire blob first record exists");
  check(first->valid(), "wire blob first record validates");
  check(first->record == blob.data() + payloadArenaOffset + presentPayloadOffset,
        "wire blob first record follows record table order, not payload offset order");
  checkEq(first->header.type, static_cast<std::uint32_t>(D9C_COMMAND_RECORD_PRESENT),
          "wire blob first record type");

  const auto second = nextImportedRecord(decoded.chunk, first->nextIndex());
  check(second.has_value(), "wire blob second record exists");
  check(second->valid(), "wire blob second record validates");
  D9CCommandRecordDrawPrimitive decodedDraw{};
  std::memcpy(&decodedDraw, second->record, sizeof(decodedDraw));
  checkEq(decodedDraw.packet.startVertex, 11u,
          "wire blob second record reads lower non-contiguous payload");

  ImportedChunkHandleSet firstHandles;
  check(collectImportedWireRecordHandles(decoded.chunk, 0u, firstHandles),
        "wire blob first record handle subset collects");
  const auto firstEntries = makeImportedChunkHandleEntries(firstHandles);
  checkEq(firstEntries.size(), static_cast<std::size_t>(1),
          "wire blob first record selects one handle");
  check(containsHandle(firstEntries, D9C_CHUNK_HANDLE_KIND_SURFACE, 0x3000u),
        "wire blob first record selects handle range from table");

  auto badBlob = blob;
  header.reserved0 = 1u;
  writeObject(badBlob, 0u, header);
  const auto badHeader = makeImportedWireChunkBlobView(
      badBlob.data(), static_cast<std::uint32_t>(badBlob.size()));
  checkEq(static_cast<int>(badHeader.status),
          static_cast<int>(ImportedWireChunkValidationStatus::InvalidChunkHeader),
          "wire blob decoder rejects nonzero chunk header reserved fields");
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
    testAllCommandIdsValidateWithExpectedRecordShapes();
    testDrawUpValidationMatrix();
    testSurfaceQueryRecordValidationMatrix();
    testInvalidTruncatedAndUnknownRecords();
    testImportedMultiRecordIteration();
    testCommandChunkWireBlobRejectsLegacyRawRecordStream();
    testImportedWireChunkRejectsMalformedPayloadRange();
    testImportedWireChunkEnforcesHandleTableAndRanges();
    testImportedWireRecordHandleRangesSelectSubsets();
    testImportedWireIterationBuildsLegacyRecordViews();
    testImportedWireAcceptsAllCommandIds();
    testImportedWireSpecialRecordValidationMatrix();
    testImportedWireDrawRunScansRecordTableOrder();
    testImportedWireBlobViewUsesHeaderTablesAndArena();
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
