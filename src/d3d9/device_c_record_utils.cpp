#include "device_c_record_utils.hpp"
#include "device_c_common.hpp"

#include <array>
#include <algorithm>
#include <cstddef>
#include <cstring>

namespace dxmt9::d3d9::devicec {
namespace {

enum class RecordSizeMode : std::uint8_t {
  Exact,
  Minimum,
  ClearRects,
  SetConst,
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

constexpr std::array<RecordLayout, 19> kRecordLayouts{{
    {D9C_COMMAND_RECORD_DRAW_PRIMITIVE,
     byteSize(sizeof(D9CCommandRecordDrawPrimitive)), RecordSizeMode::Exact},
    {D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE,
     byteSize(sizeof(D9CCommandRecordDrawIndexedPrimitive)), RecordSizeMode::Exact},
    {D9C_COMMAND_RECORD_DRAW_PRIMITIVE_UP,
     byteSize(sizeof(D9CCommandRecordDrawPrimitiveUP)), RecordSizeMode::Minimum},
    {D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE_UP,
     byteSize(sizeof(D9CCommandRecordDrawIndexedPrimitiveUP)), RecordSizeMode::Minimum},
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

bool importedDrawRecordHasNoStateDelta(
    const ImportedRecordView& record) noexcept {
  if (!record.valid() || !record.record) {
    return false;
  }

  switch (record.header.type) {
  case D9C_COMMAND_RECORD_DRAW_PRIMITIVE: {
    D9CCommandRecordDrawPrimitive decoded{};
    std::memcpy(&decoded, record.record, sizeof(decoded));
    return packetHasNoStateDelta(decoded.packet);
  }
  case D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE: {
    D9CCommandRecordDrawIndexedPrimitive decoded{};
    std::memcpy(&decoded, record.record, sizeof(decoded));
    return packetHasNoStateDelta(decoded.packet.state);
  }
  default:
    return false;
  }
}

std::uint64_t wireHandleValue(const D9CWireHandle& handle) noexcept {
  return static_cast<std::uint64_t>(handle.lo) |
         (static_cast<std::uint64_t>(handle.hi) << 32);
}

bool handleSetContains(
    const ImportedChunkHandleSet& handles,
    std::uint32_t kind,
    std::uint64_t handle) noexcept {
  if (handle == 0 || kind > D9C_CHUNK_HANDLE_KIND_VERTEX_DECL) {
    return false;
  }
  const auto& bucket = handles.byKind[kind];
  return std::find(bucket.begin(), bucket.end(), handle) != bucket.end();
}

bool handleSetsOverlap(
    const ImportedChunkHandleSet& a,
    const ImportedChunkHandleSet& b) noexcept {
  for (std::uint32_t kind = 0; kind < a.byKind.size(); ++kind) {
    for (const auto handle : a.byKind[kind]) {
      if (handleSetContains(b, kind, handle)) {
        return true;
      }
    }
  }
  return false;
}

bool hazardSetHasAnyAccess(const ImportedRecordResourceHazards& hazards) noexcept {
  for (const auto& bucket : hazards.reads.byKind) {
    if (!bucket.empty()) {
      return true;
    }
  }
  for (const auto& bucket : hazards.writes.byKind) {
    if (!bucket.empty()) {
      return true;
    }
  }
  return false;
}

void mergeHazardSets(
    ImportedRecordResourceHazards& dst,
    const ImportedRecordResourceHazards& src) {
  for (std::uint32_t kind = 0; kind < src.reads.byKind.size(); ++kind) {
    for (const auto handle : src.reads.byKind[kind]) {
      appendImportedChunkHandle(dst.reads, kind, handle);
    }
  }
  for (std::uint32_t kind = 0; kind < src.writes.byKind.size(); ++kind) {
    for (const auto handle : src.writes.byKind[kind]) {
      appendImportedChunkHandle(dst.writes, kind, handle);
    }
  }
}

void collectDrawPacketResourceHazards(
    const D9CDrawPrimitivePacket& packet,
    ImportedRecordResourceHazards& hazards) {
  for (std::uint32_t stage = 0; stage < D9C_DRAW_PACKET_MAX_TEXTURES; ++stage) {
    if ((packet.textureMask & (1u << stage)) != 0) {
      appendImportedChunkHandle(hazards.reads, D9C_CHUNK_HANDLE_KIND_TEXTURE,
                                wireHandleValue(packet.textures[stage]));
    }
  }

  for (std::uint32_t stream = 0; stream < D9C_DRAW_PACKET_MAX_STREAMS; ++stream) {
    if ((packet.streamSourceMask & (1u << stream)) != 0) {
      appendImportedChunkHandle(hazards.reads, D9C_CHUNK_HANDLE_KIND_BUFFER,
                                wireHandleValue(packet.streamSources[stream].buffer));
    }
  }

  for (std::uint32_t slot = 0; slot < D9C_DRAW_PACKET_MAX_RENDER_TARGETS; ++slot) {
    if ((packet.rtMask & (1u << slot)) != 0) {
      appendImportedChunkHandle(hazards.writes, D9C_CHUNK_HANDLE_KIND_SURFACE,
                                wireHandleValue(packet.rtHandles[slot]));
    }
  }

  if (packet.dsValid != 0) {
    appendImportedChunkHandle(hazards.writes, D9C_CHUNK_HANDLE_KIND_SURFACE,
                              wireHandleValue(packet.dsHandle));
  }
}

void collectIndexedDrawPacketResourceHazards(
    const D9CDrawIndexedPrimitivePacket& packet,
    ImportedRecordResourceHazards& hazards) {
  collectDrawPacketResourceHazards(packet.state, hazards);
  if (packet.ibValid != 0) {
    appendImportedChunkHandle(hazards.reads, D9C_CHUNK_HANDLE_KIND_BUFFER,
                              wireHandleValue(packet.ibHandle));
  }
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

std::optional<ImportedRecordView> nextImportedRecord(
    const ImportedChunkView& chunk,
    std::uint32_t offset,
    std::uint32_t index) noexcept {
  if (offset >= chunk.recordBytes) {
    return std::nullopt;
  }
  return makeImportedRecordView(chunk, offset, index);
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

ImportedDrawRunScan scanImportedDrawRun(
    const ImportedChunkView& chunk,
    const ImportedRecordView& firstRecord) noexcept {
  ImportedDrawRunScan scan{
      .firstRecord = firstRecord,
      .endOffset = firstRecord.offset,
      .endIndex = firstRecord.index,
  };

  if (!firstRecord.valid()) {
    scan.stop = ImportedDrawRunScanStop::InvalidRecord;
    scan.stopRecord = firstRecord;
    return scan;
  }

  if (firstRecord.header.type != D9C_COMMAND_RECORD_DRAW_PRIMITIVE &&
      firstRecord.header.type != D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE) {
    scan.stop = ImportedDrawRunScanStop::NotDrawRecord;
    scan.stopRecord = firstRecord;
    return scan;
  }

  scan.recordType = firstRecord.header.type;
  if (!importedDrawRecordHasNoStateDelta(firstRecord)) {
    scan.stop = ImportedDrawRunScanStop::FirstRecordHasStateDelta;
    scan.stopRecord = firstRecord;
    return scan;
  }

  scan.recordCount = 1u;
  scan.endOffset = firstRecord.nextOffset();
  scan.endIndex = firstRecord.nextIndex();
  while (auto record = nextImportedRecord(chunk, scan.endOffset, scan.endIndex)) {
    if (!record->valid()) {
      scan.stop = ImportedDrawRunScanStop::InvalidRecord;
      scan.stopRecord = *record;
      return scan;
    }
    if (record->header.type != scan.recordType) {
      scan.stop = ImportedDrawRunScanStop::DifferentRecordType;
      scan.stopRecord = *record;
      return scan;
    }
    if (!importedDrawRecordHasNoStateDelta(*record)) {
      scan.stop = ImportedDrawRunScanStop::StateDelta;
      scan.stopRecord = *record;
      return scan;
    }
    ++scan.recordCount;
    scan.endOffset = record->nextOffset();
    scan.endIndex = record->nextIndex();
  }

  scan.stop = ImportedDrawRunScanStop::EndOfChunk;
  return scan;
}

ImportedRecordReplayInfo replayInfoForCommandRecordType(std::uint32_t type) noexcept {
  switch (type) {
  case D9C_COMMAND_RECORD_DRAW_PRIMITIVE:
  case D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE:
  case D9C_COMMAND_RECORD_DRAW_PRIMITIVE_UP:
  case D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE_UP:
    return ImportedRecordReplayInfo{
        .category = ImportedRecordReplayCategory::Draw,
        .ordered = true,
        .mutatesDeviceState = true,
        .readsDeviceState = true,
        .referencesResources = true,
        .draw = true,
    };
  case D9C_COMMAND_RECORD_SET_VS_CONST_F:
  case D9C_COMMAND_RECORD_SET_VS_CONST_I:
  case D9C_COMMAND_RECORD_SET_VS_CONST_B:
  case D9C_COMMAND_RECORD_SET_PS_CONST_F:
  case D9C_COMMAND_RECORD_SET_PS_CONST_I:
  case D9C_COMMAND_RECORD_SET_PS_CONST_B:
    return ImportedRecordReplayInfo{
        .category = ImportedRecordReplayCategory::ConstantUpload,
        .ordered = true,
        .mutatesDeviceState = true,
    };
  case D9C_COMMAND_RECORD_APPLY_STATE:
    return ImportedRecordReplayInfo{
        .category = ImportedRecordReplayCategory::StateApply,
        .ordered = true,
        .mutatesDeviceState = true,
        .referencesResources = true,
    };
  case D9C_COMMAND_RECORD_CLEAR:
    return ImportedRecordReplayInfo{
        .category = ImportedRecordReplayCategory::Clear,
        .ordered = true,
        .readsDeviceState = true,
        .barrier = true,
    };
  case D9C_COMMAND_RECORD_PRESENT:
    return ImportedRecordReplayInfo{
        .category = ImportedRecordReplayCategory::Present,
        .ordered = true,
        .readsDeviceState = true,
        .barrier = true,
    };
  case D9C_COMMAND_RECORD_STRETCH_RECT:
  case D9C_COMMAND_RECORD_COLOR_FILL:
  case D9C_COMMAND_RECORD_UPDATE_TEXTURE:
  case D9C_COMMAND_RECORD_UPDATE_SURFACE:
    return ImportedRecordReplayInfo{
        .category = ImportedRecordReplayCategory::SurfaceOp,
        .ordered = true,
        .readsDeviceState = true,
        .referencesResources = true,
        .barrier = true,
    };
  case D9C_COMMAND_RECORD_QUERY_ISSUE:
    return ImportedRecordReplayInfo{
        .category = ImportedRecordReplayCategory::QueryIssue,
        .ordered = true,
        .barrier = true,
    };
  case D9C_COMMAND_RECORD_READBACK:
    return ImportedRecordReplayInfo{
        .category = ImportedRecordReplayCategory::Readback,
        .ordered = true,
        .readsDeviceState = true,
        .referencesResources = true,
        .barrier = true,
        .synchronousReadBoundary = true,
    };
  default:
    return ImportedRecordReplayInfo{};
  }
}

ImportedRecordReplayInfo replayInfoForImportedRecord(const ImportedRecordView& record) noexcept {
  if (!record.valid()) {
    return ImportedRecordReplayInfo{};
  }
  return replayInfoForCommandRecordType(record.header.type);
}

bool appendImportedChunkHandle(
    ImportedChunkHandleSet& handles,
    std::uint32_t kind,
    std::uint64_t handle) {
  if (handle == 0 || kind > D9C_CHUNK_HANDLE_KIND_VERTEX_DECL) {
    return false;
  }
  auto& bucket = handles.byKind[kind];
  if (std::find(bucket.begin(), bucket.end(), handle) != bucket.end()) {
    return false;
  }
  bucket.push_back(handle);
  return true;
}

void collectDrawPacketResourceHandles(
    const D9CDrawPrimitivePacket& packet,
    ImportedChunkHandleSet& handles) {
  for (std::uint32_t stage = 0; stage < D9C_DRAW_PACKET_MAX_TEXTURES; ++stage) {
    if ((packet.textureMask & (1u << stage)) != 0) {
      appendImportedChunkHandle(handles, D9C_CHUNK_HANDLE_KIND_TEXTURE,
                                wireHandleValue(packet.textures[stage]));
    }
  }

  for (std::uint32_t stream = 0; stream < D9C_DRAW_PACKET_MAX_STREAMS; ++stream) {
    if ((packet.streamSourceMask & (1u << stream)) != 0) {
      appendImportedChunkHandle(handles, D9C_CHUNK_HANDLE_KIND_BUFFER,
                                wireHandleValue(packet.streamSources[stream].buffer));
    }
  }

  for (std::uint32_t slot = 0; slot < D9C_DRAW_PACKET_MAX_RENDER_TARGETS; ++slot) {
    if ((packet.rtMask & (1u << slot)) != 0) {
      appendImportedChunkHandle(handles, D9C_CHUNK_HANDLE_KIND_SURFACE,
                                wireHandleValue(packet.rtHandles[slot]));
    }
  }

  if (packet.dsValid != 0) {
    appendImportedChunkHandle(handles, D9C_CHUNK_HANDLE_KIND_SURFACE,
                              wireHandleValue(packet.dsHandle));
  }
}

void collectIndexedDrawPacketResourceHandles(
    const D9CDrawIndexedPrimitivePacket& packet,
    ImportedChunkHandleSet& handles) {
  collectDrawPacketResourceHandles(packet.state, handles);
  if (packet.ibValid != 0) {
    appendImportedChunkHandle(handles, D9C_CHUNK_HANDLE_KIND_BUFFER,
                              wireHandleValue(packet.ibHandle));
  }
}

void collectImportedRecordResourceHandles(
    const ImportedRecordView& record,
    ImportedChunkHandleSet& handles) {
  if (!record.valid() || !record.record) {
    return;
  }

  switch (record.header.type) {
  case D9C_COMMAND_RECORD_DRAW_PRIMITIVE: {
    D9CCommandRecordDrawPrimitive decoded{};
    std::memcpy(&decoded, record.record, sizeof(decoded));
    collectDrawPacketResourceHandles(decoded.packet, handles);
    break;
  }
  case D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE: {
    D9CCommandRecordDrawIndexedPrimitive decoded{};
    std::memcpy(&decoded, record.record, sizeof(decoded));
    collectIndexedDrawPacketResourceHandles(decoded.packet, handles);
    break;
  }
  case D9C_COMMAND_RECORD_DRAW_PRIMITIVE_UP: {
    D9CCommandRecordDrawPrimitiveUP decoded{};
    std::memcpy(&decoded, record.record, sizeof(decoded));
    collectDrawPacketResourceHandles(decoded.packet.state, handles);
    break;
  }
  case D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE_UP: {
    D9CCommandRecordDrawIndexedPrimitiveUP decoded{};
    std::memcpy(&decoded, record.record, sizeof(decoded));
    collectDrawPacketResourceHandles(decoded.packet.state, handles);
    break;
  }
  case D9C_COMMAND_RECORD_APPLY_STATE: {
    D9CCommandRecordApplyState decoded{};
    std::memcpy(&decoded, record.record, sizeof(decoded));
    collectDrawPacketResourceHandles(decoded.packet, handles);
    break;
  }
  case D9C_COMMAND_RECORD_STRETCH_RECT: {
    D9CCommandRecordStretchRect decoded{};
    std::memcpy(&decoded, record.record, sizeof(decoded));
    appendImportedChunkHandle(handles, D9C_CHUNK_HANDLE_KIND_SURFACE, decoded.srcWire);
    appendImportedChunkHandle(handles, D9C_CHUNK_HANDLE_KIND_SURFACE, decoded.dstWire);
    break;
  }
  case D9C_COMMAND_RECORD_COLOR_FILL: {
    D9CCommandRecordColorFill decoded{};
    std::memcpy(&decoded, record.record, sizeof(decoded));
    appendImportedChunkHandle(handles, D9C_CHUNK_HANDLE_KIND_SURFACE, decoded.surfaceWire);
    break;
  }
  case D9C_COMMAND_RECORD_UPDATE_TEXTURE: {
    D9CCommandRecordUpdateTexture decoded{};
    std::memcpy(&decoded, record.record, sizeof(decoded));
    appendImportedChunkHandle(handles, D9C_CHUNK_HANDLE_KIND_TEXTURE, decoded.srcWire);
    appendImportedChunkHandle(handles, D9C_CHUNK_HANDLE_KIND_TEXTURE, decoded.dstWire);
    break;
  }
  case D9C_COMMAND_RECORD_UPDATE_SURFACE: {
    D9CCommandRecordUpdateSurface decoded{};
    std::memcpy(&decoded, record.record, sizeof(decoded));
    appendImportedChunkHandle(handles, D9C_CHUNK_HANDLE_KIND_SURFACE, decoded.srcWire);
    appendImportedChunkHandle(handles, D9C_CHUNK_HANDLE_KIND_SURFACE, decoded.dstWire);
    break;
  }
  case D9C_COMMAND_RECORD_READBACK: {
    D9CCommandRecordReadback decoded{};
    std::memcpy(&decoded, record.record, sizeof(decoded));
    appendImportedChunkHandle(handles, D9C_CHUNK_HANDLE_KIND_SURFACE, decoded.srcWire);
    appendImportedChunkHandle(handles, D9C_CHUNK_HANDLE_KIND_SURFACE, decoded.dstWire);
    break;
  }
  default:
    break;
  }
}

void collectImportedRecordResourceHazards(
    const ImportedRecordView& record,
    ImportedRecordResourceHazards& hazards) {
  if (!record.valid() || !record.record) {
    return;
  }

  switch (record.header.type) {
  case D9C_COMMAND_RECORD_DRAW_PRIMITIVE: {
    D9CCommandRecordDrawPrimitive decoded{};
    std::memcpy(&decoded, record.record, sizeof(decoded));
    collectDrawPacketResourceHazards(decoded.packet, hazards);
    break;
  }
  case D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE: {
    D9CCommandRecordDrawIndexedPrimitive decoded{};
    std::memcpy(&decoded, record.record, sizeof(decoded));
    collectIndexedDrawPacketResourceHazards(decoded.packet, hazards);
    break;
  }
  case D9C_COMMAND_RECORD_DRAW_PRIMITIVE_UP: {
    D9CCommandRecordDrawPrimitiveUP decoded{};
    std::memcpy(&decoded, record.record, sizeof(decoded));
    collectDrawPacketResourceHazards(decoded.packet.state, hazards);
    break;
  }
  case D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE_UP: {
    D9CCommandRecordDrawIndexedPrimitiveUP decoded{};
    std::memcpy(&decoded, record.record, sizeof(decoded));
    collectDrawPacketResourceHazards(decoded.packet.state, hazards);
    break;
  }
  case D9C_COMMAND_RECORD_STRETCH_RECT: {
    D9CCommandRecordStretchRect decoded{};
    std::memcpy(&decoded, record.record, sizeof(decoded));
    appendImportedChunkHandle(hazards.reads, D9C_CHUNK_HANDLE_KIND_SURFACE, decoded.srcWire);
    appendImportedChunkHandle(hazards.writes, D9C_CHUNK_HANDLE_KIND_SURFACE, decoded.dstWire);
    break;
  }
  case D9C_COMMAND_RECORD_COLOR_FILL: {
    D9CCommandRecordColorFill decoded{};
    std::memcpy(&decoded, record.record, sizeof(decoded));
    appendImportedChunkHandle(hazards.writes, D9C_CHUNK_HANDLE_KIND_SURFACE, decoded.surfaceWire);
    break;
  }
  case D9C_COMMAND_RECORD_UPDATE_TEXTURE: {
    D9CCommandRecordUpdateTexture decoded{};
    std::memcpy(&decoded, record.record, sizeof(decoded));
    appendImportedChunkHandle(hazards.reads, D9C_CHUNK_HANDLE_KIND_TEXTURE, decoded.srcWire);
    appendImportedChunkHandle(hazards.writes, D9C_CHUNK_HANDLE_KIND_TEXTURE, decoded.dstWire);
    break;
  }
  case D9C_COMMAND_RECORD_UPDATE_SURFACE: {
    D9CCommandRecordUpdateSurface decoded{};
    std::memcpy(&decoded, record.record, sizeof(decoded));
    appendImportedChunkHandle(hazards.reads, D9C_CHUNK_HANDLE_KIND_SURFACE, decoded.srcWire);
    appendImportedChunkHandle(hazards.writes, D9C_CHUNK_HANDLE_KIND_SURFACE, decoded.dstWire);
    break;
  }
  case D9C_COMMAND_RECORD_READBACK: {
    D9CCommandRecordReadback decoded{};
    std::memcpy(&decoded, record.record, sizeof(decoded));
    appendImportedChunkHandle(hazards.reads, D9C_CHUNK_HANDLE_KIND_SURFACE, decoded.srcWire);
    appendImportedChunkHandle(hazards.writes, D9C_CHUNK_HANDLE_KIND_SURFACE, decoded.dstWire);
    break;
  }
  default:
    break;
  }
}

bool importedRecordHazardsOverlap(
    const ImportedReplayHazardState& active,
    const ImportedRecordResourceHazards& record,
    bool* readAfterWrite,
    bool* writeAfterRead,
    bool* writeAfterWrite) noexcept {
  const bool raw = active.active && handleSetsOverlap(record.reads, active.hazards.writes);
  const bool war = active.active && handleSetsOverlap(record.writes, active.hazards.reads);
  const bool waw = active.active && handleSetsOverlap(record.writes, active.hazards.writes);
  if (readAfterWrite) {
    *readAfterWrite = raw;
  }
  if (writeAfterRead) {
    *writeAfterRead = war;
  }
  if (writeAfterWrite) {
    *writeAfterWrite = waw;
  }
  return raw || war || waw;
}

ImportedReplayOrderingDecision evaluateImportedReplayOrdering(
    const ImportedRecordView& record,
    const ImportedReplayHazardState& active) noexcept {
  ImportedReplayOrderingDecision decision{};
  decision.replayInfo = replayInfoForImportedRecord(record);
  if (!record.valid()) {
    decision.action = ImportedReplayOrderingAction::InvalidRecord;
    decision.resetsActiveHazards = true;
    return decision;
  }

  collectImportedRecordResourceHazards(record, decision.recordHazards);
  importedRecordHazardsOverlap(active, decision.recordHazards,
                               &decision.readAfterWrite,
                               &decision.writeAfterRead,
                               &decision.writeAfterWrite);

  if (decision.replayInfo.synchronousReadBoundary) {
    decision.action = ImportedReplayOrderingAction::SynchronousReadBoundary;
    decision.resetsActiveHazards = true;
  } else if (decision.replayInfo.barrier) {
    decision.action = ImportedReplayOrderingAction::BarrierBoundary;
    decision.resetsActiveHazards = true;
  } else if (decision.readAfterWrite || decision.writeAfterRead ||
             decision.writeAfterWrite) {
    decision.action = ImportedReplayOrderingAction::HazardBoundary;
  } else {
    decision.action = ImportedReplayOrderingAction::Continue;
  }
  return decision;
}

ImportedReplayHazardState nextImportedReplayHazardState(
    const ImportedReplayHazardState& active,
    const ImportedReplayOrderingDecision& decision) {
  if (decision.action == ImportedReplayOrderingAction::InvalidRecord ||
      decision.resetsActiveHazards) {
    return ImportedReplayHazardState{};
  }
  ImportedReplayHazardState next{};
  next.active = hazardSetHasAnyAccess(decision.recordHazards);
  next.hazards = decision.recordHazards;
  if (decision.action == ImportedReplayOrderingAction::Continue && active.active) {
    next.hazards = active.hazards;
    mergeHazardSets(next.hazards, decision.recordHazards);
    next.active = hazardSetHasAnyAccess(next.hazards);
  }
  return next;
}

std::vector<D9CChunkHandleEntry> makeImportedChunkHandleEntries(
    const ImportedChunkHandleSet& handles) {
  std::vector<D9CChunkHandleEntry> entries;
  std::size_t total = 0;
  for (const auto& bucket : handles.byKind) {
    total += bucket.size();
  }
  entries.reserve(total);
  for (std::uint32_t kind = 0; kind < handles.byKind.size(); ++kind) {
    for (const auto handle : handles.byKind[kind]) {
      entries.push_back(D9CChunkHandleEntry{
          .kind = kind,
          .reserved = 0,
          .handle = handle,
      });
    }
  }
  return entries;
}

bool packetHasNoStateDelta(const D9CDrawPrimitivePacket& p) noexcept {
  return p.renderStateCount == 0 && p.textureMask == 0 &&
         p.streamSourceMask == 0 && p.fvfValid == 0 &&
         p.vsValid == 0 && p.psValid == 0 &&
         p.vdeclValid == 0 && p.rtMask == 0 && p.dsValid == 0 &&
         p.viewportValid == 0 && p.scissorValid == 0 &&
         p.tssCount == 0 && p.samplerStateCount == 0 &&
         p.materialValid == 0 && p.clipPlaneMask == 0 &&
         p.transformCount == 0 && p.lightSlotMask == 0 &&
         p.lightEnableValidMask == 0;
}

dxmt9::core::DrawParam makeRunParam(const D9CDrawPrimitivePacket& p) noexcept {
  dxmt9::core::DrawParam dp;
  dp.indexed = false;
  dp.primitiveType = ptFromD3D(p.primitiveType);
  dp.primitiveCount = p.primitiveCount;
  dp.startVertex = p.startVertex;
  return dp;
}

dxmt9::core::DrawParam makeRunParam(
    const D9CDrawIndexedPrimitivePacket& p) noexcept {
  dxmt9::core::DrawParam dp;
  dp.indexed = true;
  dp.primitiveType = ptFromD3D(p.state.primitiveType);
  dp.primitiveCount = p.primitiveCount;
  dp.baseVertexIndex = p.baseVertex;
  dp.startIndex = p.startIndex;
  return dp;
}

}  // namespace dxmt9::d3d9::devicec
