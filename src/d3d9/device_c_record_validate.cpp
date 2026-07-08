#include "device_c_record_utils.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>

namespace dxmt9::d3d9::devicec {
namespace {

enum class RecordSizeMode : std::uint8_t {
  Exact,
  Minimum,
  ClearRects,
  SetConst,
  // R-BACK-2.52(c): DRAW_PRIMITIVE / DRAW_INDEXED_PRIMITIVE carry a fixed
  // record plus an optional trailing inline const-delta payload area (six
  // {valid,startRegister,registerCount} headers are always present; the
  // payload bytes only exist when a section is valid). Off-path (every
  // section invalid) this computes the exact same expectedSize the prior
  // Exact mode did.
  DrawPrimitiveConstDelta,
  DrawIndexedPrimitiveConstDelta,
  // DRAW_PRIMITIVE_UP / DRAW_INDEXED_PRIMITIVE_UP already carry a trailing
  // vertex(/index) data region whose own bounds are validated later against
  // the actual record bytes (recordRangeValid in device_c_chunk_replay.cpp),
  // not here -- so the off-path behavior (no valid section) must keep the
  // prior Minimum-mode contract of accepting any header.size >= minimumSize.
  // Only when a section is folded in does this additionally pin header.size
  // to vertex-data-end + const-delta-payload.
  DrawPrimitiveUPConstDelta,
  DrawIndexedPrimitiveUPConstDelta,
};

struct RecordLayout {
  std::uint32_t type;
  std::uint32_t minimumSize;
  RecordSizeMode mode;
  std::uint32_t elementSize = 0;
};

constexpr std::uint32_t byteSize(std::size_t size) noexcept {
  return static_cast<std::uint32_t>(size);
}

bool byteRangeValid(
    std::uint32_t bufferSize,
    std::uint32_t offset,
    std::uint32_t byteCount) noexcept {
  return offset <= bufferSize && byteCount <= bufferSize - offset;
}

bool tableRangeValid(
    std::uint32_t bufferSize,
    std::uint32_t offset,
    std::uint32_t count,
    std::uint32_t entrySize,
    std::uint32_t* byteCount = nullptr) noexcept {
  const auto bytes =
      static_cast<std::uint64_t>(count) * static_cast<std::uint64_t>(entrySize);
  if (bytes > 0xffffffffull) {
    return false;
  }
  const auto bytes32 = static_cast<std::uint32_t>(bytes);
  if (byteCount) {
    *byteCount = bytes32;
  }
  return byteRangeValid(bufferSize, offset, bytes32);
}

bool sectionAligned(
    const std::uint8_t* blob,
    std::uint32_t offset,
    std::size_t alignment) noexcept {
  if (alignment <= 1u) {
    return true;
  }
  const auto address = reinterpret_cast<std::uintptr_t>(blob + offset);
  return (address % alignment) == 0u;
}

constexpr std::array<RecordLayout, 20> kRecordLayouts{{
    {D9C_COMMAND_RECORD_DRAW_PRIMITIVE,
     byteSize(sizeof(D9CCommandRecordDrawPrimitive)),
     RecordSizeMode::DrawPrimitiveConstDelta},
    {D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE,
     byteSize(sizeof(D9CCommandRecordDrawIndexedPrimitive)),
     RecordSizeMode::DrawIndexedPrimitiveConstDelta},
    {D9C_COMMAND_RECORD_DRAW_PRIMITIVE_UP,
     byteSize(sizeof(D9CCommandRecordDrawPrimitiveUP)),
     RecordSizeMode::DrawPrimitiveUPConstDelta},
    {D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE_UP,
     byteSize(sizeof(D9CCommandRecordDrawIndexedPrimitiveUP)),
     RecordSizeMode::DrawIndexedPrimitiveUPConstDelta},
    {D9C_COMMAND_RECORD_SET_VS_CONST_F,
     byteSize(sizeof(D9CCommandRecordSetConst)), RecordSizeMode::SetConst,
     byteSize(sizeof(float) * 4u)},
    {D9C_COMMAND_RECORD_SET_VS_CONST_I,
     byteSize(sizeof(D9CCommandRecordSetConst)), RecordSizeMode::SetConst,
     byteSize(sizeof(std::int32_t) * 4u)},
    {D9C_COMMAND_RECORD_SET_VS_CONST_B,
     byteSize(sizeof(D9CCommandRecordSetConst)), RecordSizeMode::SetConst,
     byteSize(sizeof(std::uint32_t))},
    {D9C_COMMAND_RECORD_SET_PS_CONST_F,
     byteSize(sizeof(D9CCommandRecordSetConst)), RecordSizeMode::SetConst,
     byteSize(sizeof(float) * 4u)},
    {D9C_COMMAND_RECORD_SET_PS_CONST_I,
     byteSize(sizeof(D9CCommandRecordSetConst)), RecordSizeMode::SetConst,
     byteSize(sizeof(std::int32_t) * 4u)},
    {D9C_COMMAND_RECORD_SET_PS_CONST_B,
     byteSize(sizeof(D9CCommandRecordSetConst)), RecordSizeMode::SetConst,
     byteSize(sizeof(std::uint32_t))},
    {D9C_COMMAND_RECORD_CLEAR,
     byteSize(sizeof(D9CCommandRecordClear)), RecordSizeMode::ClearRects},
    {D9C_COMMAND_RECORD_PRESENT,
     byteSize(sizeof(D9CCommandRecordPresent)), RecordSizeMode::Exact},
    {D9C_COMMAND_RECORD_STRETCH_RECT,
     byteSize(sizeof(D9CCommandRecordStretchRect)), RecordSizeMode::Exact},
    {D9C_COMMAND_RECORD_COLOR_FILL,
     byteSize(sizeof(D9CCommandRecordColorFill)), RecordSizeMode::Exact},
    {D9C_COMMAND_RECORD_UPDATE_TEXTURE,
     byteSize(sizeof(D9CCommandRecordUpdateTexture)), RecordSizeMode::Exact},
    {D9C_COMMAND_RECORD_UPDATE_SURFACE,
     byteSize(sizeof(D9CCommandRecordUpdateSurface)), RecordSizeMode::Exact},
    {D9C_COMMAND_RECORD_QUERY_ISSUE,
     byteSize(sizeof(D9CCommandRecordQueryIssue)), RecordSizeMode::Exact},
    {D9C_COMMAND_RECORD_READBACK,
     byteSize(sizeof(D9CCommandRecordReadback)), RecordSizeMode::Exact},
    {D9C_COMMAND_RECORD_RESZ_DEPTH_RESOLVE,
     byteSize(sizeof(D9CCommandRecordReszDepthResolve)), RecordSizeMode::Exact},
    {D9C_COMMAND_RECORD_APPLY_STATE,
     byteSize(sizeof(D9CCommandRecordApplyState)), RecordSizeMode::Exact},
}};

const RecordLayout* findRecordLayout(std::uint32_t type) noexcept {
  for (const auto& layout : kRecordLayouts) {
    if (layout.type == type) {
      return &layout;
    }
  }
  return nullptr;
}

D9CCommandRecordValidation makeResult(
    D9CCommandRecordHeader header,
    std::uint32_t availableBytes,
    std::uint32_t minimumSize,
    std::uint64_t expectedSize,
    D9CCommandRecordValidationStatus status) noexcept {
  return D9CCommandRecordValidation{
      .header = header,
      .availableBytes = availableBytes,
      .minimumSize = minimumSize,
      .expectedSize = expectedSize,
      .status = status,
  };
}

std::uint64_t wireHandleValue(const D9CWireHandle& handle) noexcept {
  return static_cast<std::uint64_t>(handle.lo) |
         (static_cast<std::uint64_t>(handle.hi) << 32);
}

template <typename T>
const T* wireHandlePtr(const D9CWireHandle& handle) noexcept {
  return reinterpret_cast<const T*>(
      static_cast<std::uintptr_t>(wireHandleValue(handle)));
}

bool importedWireHandleEntryValid(
    const D9CCommandChunkWireHandleEntry& handle) noexcept {
  // R-BACK (wire-record bounds-checkable): the generation field is either the
  // legacy NONE sentinel (producer did not stamp) or a value inside the encoded
  // generation domain. The cross-side equality check against the resolved
  // handle generation is performed at replay time after the wrapper is
  // dereferenced — see `device_c_chunk_replay.cpp` "bad-handle-generation".
  return handle.kind <= D9C_CHUNK_HANDLE_KIND_VERTEX_DECL &&
         d9c_command_chunk_wire_handle_generation_valid(handle.generation) &&
         d9c_command_chunk_wire_handle_entry_reserved_valid(&handle);
}

bool importedWireRecordHeaderValid(
    const D9CCommandChunkWireRecordHeader& record) noexcept {
  return record.flags == D9C_COMMAND_CHUNK_WIRE_RECORD_FLAG_NONE &&
         d9c_command_chunk_wire_record_reserved_valid(&record);
}

ImportedRecordView makeInvalidWireImportedRecordView(
    const D9CCommandChunkWireRecordHeader& wireRecord,
    std::uint32_t recordIndex,
    D9CCommandRecordValidationStatus status) noexcept {
  constexpr auto kHeaderSize = byteSize(sizeof(D9CCommandRecordHeader));
  const D9CCommandRecordHeader legacyHeader{
      .type = wireRecord.type,
      .size = wireRecord.payloadSize,
  };
  const auto validation =
      makeResult(legacyHeader, 0u, kHeaderSize, wireRecord.payloadSize, status);
  return ImportedRecordView{
      .record = nullptr,
      .offset = wireRecord.payloadOffset,
      .index = recordIndex,
      .header = legacyHeader,
      .validation = validation,
  };
}

}  // namespace

D9CCommandRecordValidation validateCommandRecord(
    const std::uint8_t* record,
    std::uint32_t availableBytes) noexcept {
  constexpr auto kHeaderSize = byteSize(sizeof(D9CCommandRecordHeader));
  D9CCommandRecordHeader header{};

  if (!record || availableBytes < kHeaderSize) {
    return makeResult(header, availableBytes, kHeaderSize, kHeaderSize,
                      D9CCommandRecordValidationStatus::TruncatedHeader);
  }

  std::memcpy(&header, record, sizeof(header));
  if (header.size < kHeaderSize) {
    return makeResult(header, availableBytes, kHeaderSize, kHeaderSize,
                      D9CCommandRecordValidationStatus::SizeTooSmall);
  }
  if (header.size > availableBytes) {
    return makeResult(header, availableBytes, kHeaderSize, header.size,
                      D9CCommandRecordValidationStatus::TruncatedRecord);
  }

  const auto* layout = findRecordLayout(header.type);
  if (!layout) {
    return makeResult(header, availableBytes, kHeaderSize, header.size,
                      D9CCommandRecordValidationStatus::UnknownType);
  }

  const auto minimumSize = layout->minimumSize;
  if (header.size < minimumSize) {
    return makeResult(header, availableBytes, minimumSize, minimumSize,
                      D9CCommandRecordValidationStatus::SizeTooSmall);
  }

  std::uint64_t expectedSize = minimumSize;
  switch (layout->mode) {
  case RecordSizeMode::Exact:
    break;
  case RecordSizeMode::Minimum:
    expectedSize = header.size;
    break;
  case RecordSizeMode::ClearRects: {
    D9CCommandRecordClear decoded{};
    std::memcpy(&decoded, record, sizeof(decoded));
    expectedSize = static_cast<std::uint64_t>(sizeof(D9CCommandRecordClear)) +
                   static_cast<std::uint64_t>(decoded.rectCount) * sizeof(D9CRect);
    if (decoded.rectOffset != sizeof(D9CCommandRecordClear)) {
      return makeResult(header, availableBytes, minimumSize, expectedSize,
                        D9CCommandRecordValidationStatus::SizeMismatch);
    }
    break;
  }
  case RecordSizeMode::SetConst: {
    D9CCommandRecordSetConst decoded{};
    std::memcpy(&decoded, record, sizeof(decoded));
    expectedSize = static_cast<std::uint64_t>(sizeof(D9CCommandRecordSetConst)) +
                   static_cast<std::uint64_t>(decoded.count) * layout->elementSize;
    break;
  }
  case RecordSizeMode::DrawPrimitiveConstDelta: {
    D9CCommandRecordDrawPrimitive decoded{};
    std::memcpy(&decoded, record, sizeof(decoded));
    // R-BACK-2.52(c): register range must be validated before any
    // registerCount is ever trusted for a byte-count computation (an
    // unchecked registerCount could overflow the u32 payload-byte math).
    if (!d9c_draw_packet_const_delta_sections_valid(&decoded.packet)) {
      return makeResult(header, availableBytes, minimumSize, minimumSize,
                        D9CCommandRecordValidationStatus::SizeMismatch);
    }
    expectedSize = d9c_command_record_draw_primitive_total_size(&decoded.packet);
    break;
  }
  case RecordSizeMode::DrawIndexedPrimitiveConstDelta: {
    D9CCommandRecordDrawIndexedPrimitive decoded{};
    std::memcpy(&decoded, record, sizeof(decoded));
    if (!d9c_draw_packet_const_delta_sections_valid(&decoded.packet.state)) {
      return makeResult(header, availableBytes, minimumSize, minimumSize,
                        D9CCommandRecordValidationStatus::SizeMismatch);
    }
    expectedSize =
        d9c_command_record_draw_indexed_primitive_total_size(&decoded.packet.state);
    break;
  }
  case RecordSizeMode::DrawPrimitiveUPConstDelta: {
    D9CCommandRecordDrawPrimitiveUP decoded{};
    std::memcpy(&decoded, record, sizeof(decoded));
    if (!d9c_draw_packet_const_delta_sections_valid(&decoded.packet.state)) {
      return makeResult(header, availableBytes, minimumSize, minimumSize,
                        D9CCommandRecordValidationStatus::SizeMismatch);
    }
    if (d9c_draw_packet_const_delta_payload_bytes(&decoded.packet.state) != 0u) {
      expectedSize =
          d9c_command_record_draw_primitive_up_total_size(&decoded.packet);
    } else {
      // R-BACK-2.52(a): off path keeps the pre-change Minimum-mode
      // contract exactly -- any header.size >= minimumSize is accepted.
      expectedSize = header.size;
    }
    break;
  }
  case RecordSizeMode::DrawIndexedPrimitiveUPConstDelta: {
    D9CCommandRecordDrawIndexedPrimitiveUP decoded{};
    std::memcpy(&decoded, record, sizeof(decoded));
    if (!d9c_draw_packet_const_delta_sections_valid(&decoded.packet.state)) {
      return makeResult(header, availableBytes, minimumSize, minimumSize,
                        D9CCommandRecordValidationStatus::SizeMismatch);
    }
    if (d9c_draw_packet_const_delta_payload_bytes(&decoded.packet.state) != 0u) {
      expectedSize =
          d9c_command_record_draw_indexed_primitive_up_total_size(&decoded.packet);
    } else {
      expectedSize = header.size;
    }
    break;
  }
  }

  if (header.size != expectedSize) {
    return makeResult(header, availableBytes, minimumSize, expectedSize,
                      D9CCommandRecordValidationStatus::SizeMismatch);
  }

  return makeResult(header, availableBytes, minimumSize, expectedSize,
                    D9CCommandRecordValidationStatus::Valid);
}

const char* commandRecordValidationStatusName(
    D9CCommandRecordValidationStatus status) noexcept {
  switch (status) {
  case D9CCommandRecordValidationStatus::Valid:
    return "valid";
  case D9CCommandRecordValidationStatus::TruncatedHeader:
    return "truncated header";
  case D9CCommandRecordValidationStatus::SizeTooSmall:
    return "size too small";
  case D9CCommandRecordValidationStatus::TruncatedRecord:
    return "truncated record";
  case D9CCommandRecordValidationStatus::UnknownType:
    return "unknown type";
  case D9CCommandRecordValidationStatus::SizeMismatch:
    return "size mismatch";
  }
  return "unknown status";
}

ImportedChunkView makeImportedChunkView(
    const std::uint8_t* records,
    std::uint32_t recordBytes,
    std::uint32_t recordCount) noexcept {
  return ImportedChunkView{
      .records = records,
      .recordBytes = recordBytes,
      .recordCount = recordCount,
  };
}

ImportedChunkView makeImportedChunkView(
    const D9CCommandChunk& chunk) noexcept {
  const auto* records = chunk.recordBytes != 0
                            ? wireHandlePtr<const std::uint8_t>(chunk.records)
                            : nullptr;
  return makeImportedChunkView(records, chunk.recordBytes, chunk.recordCount);
}

ImportedWireChunkView makeImportedWireChunkView(
    const D9CCommandChunkWireRecordHeader* records,
    std::uint32_t recordCount,
    const std::uint8_t* payloadArena,
    std::uint32_t payloadArenaSize,
    const D9CCommandChunkWireHandleEntry* handles,
    std::uint32_t handleCount) noexcept {
  return ImportedWireChunkView{
      .records = records,
      .recordCount = recordCount,
      .payloadArena = payloadArena,
      .payloadArenaSize = payloadArenaSize,
      .handles = handles,
      .handleCount = handleCount,
  };
}

ImportedWireChunkBlobView makeImportedWireChunkBlobView(
    const std::uint8_t* blob,
    std::uint32_t blobSize) noexcept {
  ImportedWireChunkBlobView result{};
  if (!blob || blobSize < sizeof(D9CCommandChunkWireHeader)) {
    result.status = ImportedWireChunkValidationStatus::MissingChunkHeader;
    return result;
  }

  std::memcpy(&result.header, blob, sizeof(result.header));
  result.headerPresent = true;
  const auto& header = result.header;

  if (header.version != D9C_COMMAND_CHUNK_WIRE_VERSION ||
      header.headerSize != D9C_COMMAND_CHUNK_WIRE_HEADER_SIZE ||
      header.recordHeaderSize != D9C_COMMAND_CHUNK_WIRE_RECORD_HEADER_SIZE ||
      header.handleEntrySize != D9C_COMMAND_CHUNK_WIRE_HANDLE_ENTRY_SIZE ||
      !d9c_command_chunk_wire_header_reserved_valid(&header) ||
      !byteRangeValid(blobSize, 0u, header.headerSize)) {
    result.status = ImportedWireChunkValidationStatus::InvalidChunkHeader;
    return result;
  }

  std::uint32_t recordTableBytes = 0u;
  std::uint32_t handleTableBytes = 0u;
  if (!tableRangeValid(blobSize, header.recordTableOffset, header.recordCount,
                       header.recordHeaderSize, &recordTableBytes) ||
      !tableRangeValid(blobSize, header.handleTableOffset, header.handleCount,
                       header.handleEntrySize, &handleTableBytes) ||
      !byteRangeValid(blobSize, header.payloadArenaOffset,
                      header.payloadArenaSize)) {
    result.status = ImportedWireChunkValidationStatus::InvalidChunkRange;
    return result;
  }

  if ((recordTableBytes != 0u &&
       (header.recordTableOffset < header.headerSize ||
        !sectionAligned(blob, header.recordTableOffset,
                        alignof(D9CCommandChunkWireRecordHeader)))) ||
      (handleTableBytes != 0u &&
       (header.handleTableOffset < header.headerSize ||
        !sectionAligned(blob, header.handleTableOffset,
                        alignof(D9CCommandChunkWireHandleEntry)))) ||
      (header.payloadArenaSize != 0u &&
       header.payloadArenaOffset < header.headerSize)) {
    result.status = ImportedWireChunkValidationStatus::InvalidChunkRange;
    return result;
  }

  const auto* records = header.recordCount != 0u
                            ? reinterpret_cast<const D9CCommandChunkWireRecordHeader*>(
                                  blob + header.recordTableOffset)
                            : nullptr;
  const auto* handles = header.handleCount != 0u
                            ? reinterpret_cast<const D9CCommandChunkWireHandleEntry*>(
                                  blob + header.handleTableOffset)
                            : nullptr;
  const auto* payloadArena = header.payloadArenaSize != 0u
                                 ? blob + header.payloadArenaOffset
                                 : nullptr;

  result.chunk = makeImportedWireChunkView(
      records, header.recordCount, payloadArena, header.payloadArenaSize,
      handles, header.handleCount);
  const auto validation = validateImportedWireChunk(result.chunk);
  result.status = validation.status;
  return result;
}

ImportedRecordView makeImportedRecordView(
    const ImportedChunkView& chunk,
    std::uint32_t offset,
    std::uint32_t index) noexcept {
  const std::uint32_t availableBytes =
      offset < chunk.recordBytes ? chunk.recordBytes - offset : 0u;
  const auto* record =
      availableBytes != 0 && chunk.records ? chunk.records + offset : nullptr;
  const auto validation = validateCommandRecord(record, availableBytes);
  return ImportedRecordView{
      .record = record,
      .offset = offset,
      .index = index,
      .header = validation.header,
      .validation = validation,
  };
}

ImportedRecordView makeImportedRecordView(
    const ImportedWireChunkView& chunk,
    std::uint32_t recordIndex) noexcept {
  if (recordIndex >= chunk.recordCount || !chunk.records) {
    return makeInvalidWireImportedRecordView(
        D9CCommandChunkWireRecordHeader{}, recordIndex,
        D9CCommandRecordValidationStatus::TruncatedHeader);
  }

  const auto& wireRecord = chunk.records[recordIndex];
  if (!importedWireRecordHeaderValid(wireRecord)) {
    return makeInvalidWireImportedRecordView(
        wireRecord, recordIndex,
        D9CCommandRecordValidationStatus::UnknownType);
  }
  if (!d9c_command_chunk_wire_record_ranges_valid(
          chunk.payloadArenaSize, chunk.handleCount, &wireRecord) ||
      (wireRecord.payloadSize != 0 && !chunk.payloadArena)) {
    return makeInvalidWireImportedRecordView(
        wireRecord, recordIndex,
        D9CCommandRecordValidationStatus::TruncatedRecord);
  }

  const auto* record = wireRecord.payloadSize != 0
                           ? chunk.payloadArena + wireRecord.payloadOffset
                           : nullptr;
  auto validation = validateCommandRecord(record, wireRecord.payloadSize);
  if (validation.valid() && validation.header.size != wireRecord.payloadSize) {
    validation =
        makeResult(validation.header, wireRecord.payloadSize,
                   validation.minimumSize, validation.expectedSize,
                   D9CCommandRecordValidationStatus::SizeMismatch);
  }
  return ImportedRecordView{
      .record = record,
      .offset = wireRecord.payloadOffset,
      .index = recordIndex,
      .header = validation.header,
      .validation = validation,
  };
}

std::optional<ImportedRecordView> nextImportedRecord(
    const ImportedChunkView& chunk,
    std::uint32_t offset,
    std::uint32_t index) noexcept {
  if (offset >= chunk.recordBytes) {
    return std::nullopt;
  }
  return makeImportedRecordView(chunk, offset, index);
}

std::optional<ImportedRecordView> nextImportedRecord(
    const ImportedWireChunkView& chunk,
    std::uint32_t recordIndex) noexcept {
  if (recordIndex >= chunk.recordCount) {
    return std::nullopt;
  }
  return makeImportedRecordView(chunk, recordIndex);
}

ImportedWireRecordHandleView makeImportedWireRecordHandleView(
    const ImportedWireChunkView& chunk,
    std::uint32_t recordIndex) noexcept {
  if (recordIndex >= chunk.recordCount || !chunk.records) {
    return ImportedWireRecordHandleView{};
  }

  const auto& record = chunk.records[recordIndex];
  if (!d9c_command_chunk_wire_handle_range_valid(
          chunk.handleCount, record.firstHandle, record.handleCount) ||
      (record.handleCount != 0u && !chunk.handles)) {
    return ImportedWireRecordHandleView{};
  }

  return ImportedWireRecordHandleView{
      .handles = record.handleCount != 0u
                     ? chunk.handles + record.firstHandle
                     : nullptr,
      .firstHandle = record.firstHandle,
      .handleCount = record.handleCount,
  };
}

bool collectImportedWireRecordHandles(
    const ImportedWireChunkView& chunk,
    std::uint32_t recordIndex,
    ImportedChunkHandleSet& handles) {
  if (recordIndex >= chunk.recordCount || !chunk.records) {
    return false;
  }

  const auto& record = chunk.records[recordIndex];
  const auto view = makeImportedWireRecordHandleView(chunk, recordIndex);
  if (view.handleCount != record.handleCount ||
      view.firstHandle != record.firstHandle ||
      (view.handleCount != 0u && !view.handles)) {
    return false;
  }

  for (std::uint32_t i = 0; i < view.handleCount; ++i) {
    const auto& handle = view.handles[i];
    if (!importedWireHandleEntryValid(handle)) {
      return false;
    }
    appendImportedChunkHandle(handles, handle.kind, handle.opaqueHandle);
  }
  return true;
}

bool collectImportedWireChunkHandles(
    const ImportedWireChunkView& chunk,
    ImportedChunkHandleSet& handles) {
  if (chunk.recordCount != 0u && !chunk.records) {
    return false;
  }
  for (std::uint32_t i = 0; i < chunk.recordCount; ++i) {
    if (!collectImportedWireRecordHandles(chunk, i, handles)) {
      return false;
    }
  }
  return true;
}

bool importedChunkRecordCountMatches(
    const ImportedChunkView& chunk,
    std::uint32_t actualRecordCount) noexcept {
  return actualRecordCount == chunk.recordCount;
}

ImportedChunkValidation validateImportedChunk(
    const ImportedChunkView& chunk) noexcept {
  ImportedChunkValidation result{
      .chunk = chunk,
      .status = ImportedChunkValidationStatus::Valid,
  };

  std::uint32_t offset = 0;
  std::uint32_t index = 0;
  while (auto record = nextImportedRecord(chunk, offset, index)) {
    if (!record->valid()) {
      result.status = ImportedChunkValidationStatus::InvalidRecord;
      result.consumedBytes = offset;
      result.parsedRecordCount = index;
      result.failedRecord = *record;
      return result;
    }
    offset = record->nextOffset();
    index = record->nextIndex();
  }

  result.consumedBytes = offset;
  result.parsedRecordCount = index;
  if (!importedChunkRecordCountMatches(chunk, index)) {
    result.status = ImportedChunkValidationStatus::RecordCountMismatch;
  }
  return result;
}

ImportedWireChunkValidation validateImportedWireChunk(
    const ImportedWireChunkView& chunk) noexcept {
  ImportedWireChunkValidation result{
      .chunk = chunk,
      .status = ImportedWireChunkValidationStatus::Valid,
  };

  if (chunk.recordCount != 0 && !chunk.records) {
    result.status = ImportedWireChunkValidationStatus::MissingRecordTable;
    return result;
  }
  if (chunk.payloadArenaSize != 0 && !chunk.payloadArena) {
    result.status = ImportedWireChunkValidationStatus::MissingPayloadArena;
    return result;
  }
  if (chunk.handleCount != 0 && !chunk.handles) {
    result.status = ImportedWireChunkValidationStatus::MissingHandleTable;
    return result;
  }

  for (std::uint32_t i = 0; i < chunk.handleCount; ++i) {
    if (!importedWireHandleEntryValid(chunk.handles[i])) {
      result.status = ImportedWireChunkValidationStatus::InvalidHandleEntry;
      result.failedHandleIndex = i;
      result.failedHandle = chunk.handles[i];
      return result;
    }
  }

  for (std::uint32_t i = 0; i < chunk.recordCount; ++i) {
    const auto& wireRecord = chunk.records[i];
    if (!importedWireRecordHeaderValid(wireRecord)) {
      result.status = ImportedWireChunkValidationStatus::InvalidRecordHeader;
      result.failedRecordIndex = i;
      result.failedWireRecord = wireRecord;
      return result;
    }
    if (!d9c_command_chunk_wire_record_ranges_valid(
            chunk.payloadArenaSize, chunk.handleCount, &wireRecord)) {
      result.status = ImportedWireChunkValidationStatus::InvalidRecordRange;
      result.failedRecordIndex = i;
      result.failedWireRecord = wireRecord;
      return result;
    }

    const auto record = makeImportedRecordView(chunk, i);
    if (!record.valid() || record.header.type != wireRecord.type) {
      result.status = ImportedWireChunkValidationStatus::InvalidRecord;
      result.failedRecordIndex = i;
      result.failedWireRecord = wireRecord;
      result.failedRecord = record;
      return result;
    }
    result.parsedRecordCount = i + 1u;
  }

  return result;
}

}  // namespace dxmt9::d3d9::devicec
