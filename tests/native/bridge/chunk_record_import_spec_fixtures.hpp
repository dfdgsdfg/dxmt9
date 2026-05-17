#pragma once

#include "device_c_record_utils.hpp"

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

namespace dxmt9::d3d9::devicec::spec {

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

[[noreturn]] inline void fail(std::string message) {
  throw TestFailure(std::move(message));
}

inline void check(bool condition, std::string_view message) {
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

inline D9CCommandRecordPresent makePresentRecord() {
  D9CCommandRecordPresent present{};
  present.header.type = D9C_COMMAND_RECORD_PRESENT;
  present.header.size = sizeof(present);
  return present;
}

inline D9CCommandRecordDrawPrimitive makeDrawPrimitiveRecord(
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

inline D9CCommandRecordDrawIndexedPrimitive makeDrawIndexedPrimitiveRecord(
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

inline D9CCommandRecordDrawPrimitiveUP makeDrawPrimitiveUPRecord(
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

inline D9CCommandRecordDrawIndexedPrimitiveUP makeDrawIndexedPrimitiveUPRecord(
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

inline D9CCommandRecordSetConst makeSetConstRecord(
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

inline D9CCommandRecordClear makeClearRecord(std::uint32_t rectCount = 0u) {
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

inline D9CCommandRecordStretchRect makeStretchRectRecord() {
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

inline D9CCommandRecordColorFill makeColorFillRecord() {
  D9CCommandRecordColorFill color{};
  color.header.type = D9C_COMMAND_RECORD_COLOR_FILL;
  color.header.size = sizeof(color);
  color.surfaceWire = 0x4100u;
  color.colorARGB = 0xff0000ffu;
  color.hasRect = 1u;
  color.rect = D9CRect{0, 0, 8, 8};
  return color;
}

inline D9CCommandRecordUpdateTexture makeUpdateTextureRecord() {
  D9CCommandRecordUpdateTexture update{};
  update.header.type = D9C_COMMAND_RECORD_UPDATE_TEXTURE;
  update.header.size = sizeof(update);
  update.srcWire = 0x5000u;
  update.dstWire = 0x5008u;
  return update;
}

inline D9CCommandRecordUpdateSurface makeUpdateSurfaceRecord() {
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

inline D9CCommandRecordQueryIssue makeQueryIssueRecord() {
  D9CCommandRecordQueryIssue query{};
  query.header.type = D9C_COMMAND_RECORD_QUERY_ISSUE;
  query.header.size = sizeof(query);
  query.queryWire = 0x5200u;
  query.flags = 1u;
  return query;
}

inline D9CCommandRecordReadback makeReadbackRecord() {
  D9CCommandRecordReadback readback{};
  readback.header.type = D9C_COMMAND_RECORD_READBACK;
  readback.header.size = sizeof(readback);
  readback.srcWire = 0x6000u;
  readback.dstWire = 0x6008u;
  return readback;
}

inline D9CCommandRecordApplyState makeApplyStateRecord() {
  D9CCommandRecordApplyState apply{};
  apply.header.type = D9C_COMMAND_RECORD_APPLY_STATE;
  apply.header.size = sizeof(apply);
  apply.packet.renderStateCount = 1u;
  apply.packet.renderStates[0].state = 7u;
  apply.packet.renderStates[0].value = 9u;
  return apply;
}

inline D9CWireHandle wireHandle(std::uint64_t value) {
  return D9CWireHandle{
      .lo = static_cast<std::uint32_t>(value),
      .hi = static_cast<std::uint32_t>(value >> 32),
  };
}

inline D9CCommandChunkWireHandleEntry wireHandleEntry(
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

inline D9CCommandChunkWireRecordHeader wireRecordHeader(
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

inline D9CCommandRecordDrawPrimitive makeHazardDrawRecord(
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

inline bool containsHandle(const std::vector<D9CChunkHandleEntry>& entries,
                           std::uint32_t kind,
                           std::uint64_t handle) {
  for (const auto& entry : entries) {
    if (entry.kind == kind && entry.handle == handle) {
      return true;
    }
  }
  return false;
}

inline void checkStatus(const std::vector<std::uint8_t>& bytes,
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

inline void checkValidRecordBytes(const std::vector<std::uint8_t>& bytes,
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

inline void checkWireRecordMatrix(const std::vector<std::uint8_t>& payload,
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

  // In-range stamped generations are now structurally valid (the
  // cross-side equality check moved into chunk replay). The wire
  // validator only flags generations that overflow the encoded domain.
  badHandles = handles;
  badHandles[0].generation = 1u;
  wire = makeImportedWireChunkView(
      records.data(), static_cast<std::uint32_t>(records.size()),
      payload.data(), static_cast<std::uint32_t>(payload.size()),
      badHandles.data(), static_cast<std::uint32_t>(badHandles.size()));
  validation = validateImportedWireChunk(wire);
  checkEq(static_cast<int>(validation.status),
          static_cast<int>(ImportedWireChunkValidationStatus::Valid),
          std::string(name) + " accepts in-range stamped generation");

  badHandles = handles;
  badHandles[0].generation =
      D9C_COMMAND_CHUNK_WIRE_HANDLE_GENERATION_MASK + 1u;
  wire = makeImportedWireChunkView(
      records.data(), static_cast<std::uint32_t>(records.size()),
      payload.data(), static_cast<std::uint32_t>(payload.size()),
      badHandles.data(), static_cast<std::uint32_t>(badHandles.size()));
  validation = validateImportedWireChunk(wire);
  checkEq(static_cast<int>(validation.status),
          static_cast<int>(ImportedWireChunkValidationStatus::InvalidHandleEntry),
          std::string(name) + " rejects out-of-range wire handle generations");
}

inline void checkWireAcceptsRecordBytes(const std::vector<std::uint8_t>& payload,
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

}  // namespace dxmt9::d3d9::devicec::spec
