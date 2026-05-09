#include "device_c_record_utils.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace dxmt9::d3d9::devicec {
namespace {

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

}  // namespace dxmt9::d3d9::devicec
