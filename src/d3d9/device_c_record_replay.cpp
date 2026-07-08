#include "device_c_record_utils.hpp"
#include "device_c_common.hpp"

#include <cstdint>
#include <cstring>

namespace dxmt9::d3d9::devicec {
namespace {

bool wireHandleEquals(const D9CWireHandle& a, const D9CWireHandle& b) noexcept {
  return a.lo == b.lo && a.hi == b.hi;
}

bool importedRecordIsDrawRunCandidate(const ImportedRecordView& record) noexcept {
  return record.header.type == D9C_COMMAND_RECORD_DRAW_PRIMITIVE ||
         record.header.type == D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE;
}

// R-BACK-2.52(f): true iff any of the packet's six inline const-delta
// sections is folded in (DXMT9_PE_INLINE_CONST_DELTA). Off-path packets
// (every section left valid=0) always return false here.
bool packetHasAnyConstDeltaSection(const D9CDrawPrimitivePacket& p) noexcept {
  for (std::uint32_t kind = 0; kind < D9C_DRAW_PACKET_CONST_DELTA_COUNT; ++kind) {
    if (p.constDeltaSections[kind].valid) {
      return true;
    }
  }
  return false;
}

bool drawPacketStateDeltaEquals(const D9CDrawPrimitivePacket& a,
                                const D9CDrawPrimitivePacket& b) noexcept {
  if (a.renderStateCount != b.renderStateCount ||
      a.textureMask != b.textureMask ||
      a.streamSourceMask != b.streamSourceMask ||
      a.fvfValid != b.fvfValid ||
      a.vsValid != b.vsValid ||
      a.psValid != b.psValid ||
      a.vdeclValid != b.vdeclValid ||
      a.rtMask != b.rtMask ||
      a.dsValid != b.dsValid ||
      a.viewportValid != b.viewportValid ||
      a.scissorValid != b.scissorValid ||
      a.tssCount != b.tssCount ||
      a.samplerStateCount != b.samplerStateCount ||
      a.materialValid != b.materialValid ||
      a.clipPlaneMask != b.clipPlaneMask ||
      a.transformCount != b.transformCount ||
      a.lightSlotMask != b.lightSlotMask ||
      a.lightEnableValidMask != b.lightEnableValidMask ||
      a.lightEnableMask != b.lightEnableMask) {
    return false;
  }

  if (std::memcmp(a.renderStates, b.renderStates,
                  sizeof(a.renderStates[0]) * a.renderStateCount) != 0 ||
      std::memcmp(a.textures, b.textures, sizeof(a.textures)) != 0 ||
      std::memcmp(a.streamSources, b.streamSources, sizeof(a.streamSources)) != 0) {
    return false;
  }
  if (a.fvfValid && a.fvf != b.fvf) return false;
  if (a.vsValid && !wireHandleEquals(a.vsHandle, b.vsHandle)) return false;
  if (a.psValid && !wireHandleEquals(a.psHandle, b.psHandle)) return false;
  if (a.vdeclValid && !wireHandleEquals(a.vdeclHandle, b.vdeclHandle)) return false;
  if (std::memcmp(a.rtHandles, b.rtHandles, sizeof(a.rtHandles)) != 0) return false;
  if (a.dsValid && !wireHandleEquals(a.dsHandle, b.dsHandle)) return false;
  if (a.viewportValid && std::memcmp(&a.viewport, &b.viewport, sizeof(a.viewport)) != 0) return false;
  if (a.scissorValid && std::memcmp(&a.scissor, &b.scissor, sizeof(a.scissor)) != 0) return false;
  if (std::memcmp(a.tss, b.tss, sizeof(a.tss[0]) * a.tssCount) != 0 ||
      std::memcmp(a.samplerStates, b.samplerStates,
                  sizeof(a.samplerStates[0]) * a.samplerStateCount) != 0) {
    return false;
  }
  if (a.materialValid && std::memcmp(&a.material, &b.material, sizeof(a.material)) != 0) return false;
  if (a.clipPlaneMask != 0 && std::memcmp(a.clipPlanes, b.clipPlanes, sizeof(a.clipPlanes)) != 0) return false;
  if (std::memcmp(a.transforms, b.transforms, sizeof(a.transforms[0]) * a.transformCount) != 0) return false;
  if (a.lightSlotMask != 0 && std::memcmp(a.lights, b.lights, sizeof(a.lights)) != 0) return false;
  return true;
}

D9CDrawPrimitivePacket drawPacketWithoutStreamDelta(
    D9CDrawPrimitivePacket packet) noexcept {
  packet.streamSourceMask = 0;
  std::memset(packet.streamSources, 0, sizeof(packet.streamSources));
  return packet;
}

bool packetHasNoStateDeltaExceptStream(const D9CDrawPrimitivePacket& p) noexcept {
  return packetHasNoStateDelta(drawPacketWithoutStreamDelta(p));
}

bool drawPacketStateDeltaEqualsExceptStream(
    const D9CDrawPrimitivePacket& a,
    const D9CDrawPrimitivePacket& b) noexcept {
  return drawPacketStateDeltaEquals(drawPacketWithoutStreamDelta(a),
                                    drawPacketWithoutStreamDelta(b));
}

bool drawPacketStateDeltaCompatibleWithRunBase(
    const D9CDrawPrimitivePacket& base,
    const D9CDrawPrimitivePacket& candidate) noexcept {
  // R-BACK-2.52(f): a candidate carrying any valid inline const-delta
  // section folds the equivalent of a standalone
  // D9C_COMMAND_RECORD_SET_*_CONST_* record into this draw. That standalone
  // record always breaks a run before reaching this draw in the unfolded
  // wire form (scanStopForNonDrawRecord below stops the scan at any
  // constant-upload record), so the folded candidate must break here too --
  // unconditionally, regardless of whether its other state fields happen to
  // equal the run base's. Section headers carry no content hash, so two
  // sections with identical {valid,startRegister,registerCount} can still
  // carry different underlying register values on the wire; only an
  // unconditional "candidate with any section always breaks" rule is sound.
  if (packetHasAnyConstDeltaSection(candidate)) {
    return false;
  }
  if (packetHasNoStateDelta(candidate)) {
    return true;
  }
  if (packetHasNoStateDeltaExceptStream(candidate)) {
    return true;
  }
  return !packetHasNoStateDelta(base) &&
         drawPacketStateDeltaEqualsExceptStream(base, candidate);
}

struct ImportedDrawRecordDelta {
  D9CDrawPrimitivePacket state{};
  bool indexed = false;
  bool ibValid = false;
  D9CWireHandle ibHandle{};
  bool valid = false;
};

ImportedDrawRecordDelta importedDrawRecordDelta(
    const ImportedRecordView& record) noexcept {
  if (!record.valid() || !record.record) {
    return {};
  }
  switch (record.header.type) {
  case D9C_COMMAND_RECORD_DRAW_PRIMITIVE: {
    D9CCommandRecordDrawPrimitive decoded{};
    std::memcpy(&decoded, record.record, sizeof(decoded));
    return ImportedDrawRecordDelta{
        .state = decoded.packet,
        .indexed = false,
        .ibValid = false,
        .ibHandle = {},
        .valid = true,
    };
  }
  case D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE: {
    D9CCommandRecordDrawIndexedPrimitive decoded{};
    std::memcpy(&decoded, record.record, sizeof(decoded));
    return ImportedDrawRecordDelta{
        .state = decoded.packet.state,
        .indexed = true,
        .ibValid = decoded.packet.ibValid != 0,
        .ibHandle = decoded.packet.ibHandle,
        .valid = true,
    };
  }
  default:
    return {};
  }
}

bool importedDrawRecordCompatibleWithRunBase(
    const ImportedRecordView& base,
    const ImportedRecordView& candidate) noexcept {
  if (!base.valid() || !candidate.valid() ||
      !importedRecordIsDrawRunCandidate(base) ||
      !importedRecordIsDrawRunCandidate(candidate)) {
    return false;
  }

  const auto baseDelta = importedDrawRecordDelta(base);
  const auto candidateDelta = importedDrawRecordDelta(candidate);
  if (!baseDelta.valid || !candidateDelta.valid ||
      !drawPacketStateDeltaCompatibleWithRunBase(baseDelta.state,
                                                candidateDelta.state)) {
    return false;
  }

  return true;
}

bool importedDrawRecordHasSameType(
    const ImportedRecordView& base,
    const ImportedRecordView& candidate) noexcept {
  return base.header.type == candidate.header.type;
}

bool importedRecordIsConstantUpload(
    const ImportedRecordView& record) noexcept {
  switch (record.header.type) {
  case D9C_COMMAND_RECORD_SET_VS_CONST_F:
  case D9C_COMMAND_RECORD_SET_VS_CONST_I:
  case D9C_COMMAND_RECORD_SET_VS_CONST_B:
  case D9C_COMMAND_RECORD_SET_PS_CONST_F:
  case D9C_COMMAND_RECORD_SET_PS_CONST_I:
  case D9C_COMMAND_RECORD_SET_PS_CONST_B:
    return true;
  default:
    return false;
  }
}

void scanStopForIncompatibleDrawRecord(
    ImportedDrawRunScan& scan,
    const ImportedRecordView& firstRecord,
    const ImportedRecordView& record) noexcept {
  scan.stop = importedDrawRecordHasSameType(firstRecord, record)
      ? ImportedDrawRunScanStop::StateDelta
      : ImportedDrawRunScanStop::DifferentRecordType;
  scan.stopRecord = record;
}

void scanStopForNonDrawRecord(
    ImportedDrawRunScan& scan,
    const ImportedRecordView& record) noexcept {
  scan.stop = importedRecordIsConstantUpload(record)
      ? ImportedDrawRunScanStop::ConstantUpload
      : ImportedDrawRunScanStop::DifferentRecordType;
  scan.stopRecord = record;
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

  if (!importedRecordIsDrawRunCandidate(firstRecord)) {
    scan.stop = ImportedDrawRunScanStop::NotDrawRecord;
    scan.stopRecord = firstRecord;
    return scan;
  }

  scan.recordType = firstRecord.header.type;
  scan.recordCount = 1u;
  scan.endOffset = firstRecord.nextOffset();
  scan.endIndex = firstRecord.nextIndex();
  while (auto record = nextImportedRecord(chunk, scan.endOffset, scan.endIndex)) {
    if (!record->valid()) {
      scan.stop = ImportedDrawRunScanStop::InvalidRecord;
      scan.stopRecord = *record;
      return scan;
    }
    if (!importedRecordIsDrawRunCandidate(*record)) {
      scanStopForNonDrawRecord(scan, *record);
      return scan;
    }
    if (!importedDrawRecordCompatibleWithRunBase(firstRecord, *record)) {
      scanStopForIncompatibleDrawRecord(scan, firstRecord, *record);
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

  if (!importedRecordIsDrawRunCandidate(firstRecord)) {
    scan.stop = ImportedDrawRunScanStop::NotDrawRecord;
    scan.stopRecord = firstRecord;
    return scan;
  }

  scan.recordType = firstRecord.header.type;
  scan.recordCount = 1u;
  scan.endOffset = firstRecord.nextOffset();
  scan.endIndex = firstRecord.nextIndex();
  while (auto record = nextImportedRecord(chunk, scan.endIndex)) {
    if (!record->valid()) {
      scan.stop = ImportedDrawRunScanStop::InvalidRecord;
      scan.stopRecord = *record;
      return scan;
    }
    if (!importedRecordIsDrawRunCandidate(*record)) {
      scanStopForNonDrawRecord(scan, *record);
      return scan;
    }
    if (!importedDrawRecordCompatibleWithRunBase(firstRecord, *record)) {
      scanStopForIncompatibleDrawRecord(scan, firstRecord, *record);
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
  // R-FORMAT-11: RESZ depth-resolve is a fire-and-forget surface op (MSAA
  // depth source → INTZ destination) — the same SurfaceOp ordering-barrier
  // class as StretchRect/ColorFill, NOT the synchronousReadBoundary Readback
  // class, so PE never blocks on a result. Classifying it here (rather than
  // letting it fall through to the empty Unknown info) keeps chunk replay
  // from misreading it as a draw.
  case D9C_COMMAND_RECORD_RESZ_DEPTH_RESOLVE:
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
  // R-BACK-2.52(f): a folded inline const-delta section is state that must
  // be applied for this draw, exactly like a standalone
  // D9C_COMMAND_RECORD_SET_*_CONST_* record would have been -- it must
  // count as state-delta-bearing so the run coalescer (and any other
  // "nothing to apply" fast path) never silently treats this packet as a
  // no-op continuation.
  return !packetHasAnyConstDeltaSection(p) &&
         p.renderStateCount == 0 && p.textureMask == 0 &&
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
