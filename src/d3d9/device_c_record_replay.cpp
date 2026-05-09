#include "device_c_record_utils.hpp"
#include "device_c_common.hpp"

#include <cstdint>
#include <cstring>

namespace dxmt9::d3d9::devicec {
namespace {

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

}  // namespace

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

ImportedDrawRunScan scanImportedDrawRun(
    const ImportedWireChunkView& chunk,
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
  while (auto record = nextImportedRecord(chunk, scan.endIndex)) {
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
