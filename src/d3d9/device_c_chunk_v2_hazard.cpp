#include "device_c_chunk_v2_replay.hpp"
#include "device_c_record_utils.hpp"

#include <algorithm>
#include <cstring>

namespace dxmt9::d3d9::devicec {

namespace {

template <typename T>
bool load(std::span<const std::byte> bytes, T& value) {
  if (bytes.size() < sizeof(T)) {
    return false;
  }
  std::memcpy(&value, bytes.data(), sizeof(T));
  return true;
}

template <typename T>
std::span<const T> typedSection(const ImportedSectionV2View& section) {
  return std::span<const T>(
      reinterpret_cast<const T*>(section.payload.data()),
      section.descriptor.count);
}

std::uint64_t objectValue(void* object) {
  return static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(object));
}

void appendObject(ImportedChunkHandleSet& handles, std::uint32_t kind,
                  void* object) {
  appendImportedChunkHandle(handles, kind, objectValue(object));
}

void collectSparseHazards(const ResolvedRecordV2View& record,
                          ImportedRecordResourceHazards& hazards) {
  for (std::size_t i = 0u; i < record.wire.sections.size(); ++i) {
    const auto section = record.wire.section(i);
    switch (section.descriptor.kind) {
      case D9C_COMMAND_CHUNK_V2_SECTION_TEXTURE:
        for (const auto& value :
             typedSection<D9CCommandChunkWireTextureBindingV2>(section)) {
          if (value.valid) {
            appendObject(hazards.reads, D9C_CHUNK_HANDLE_KIND_TEXTURE,
                         record.objectForAbsoluteIndex(value.handleIndex));
          }
        }
        break;
      case D9C_COMMAND_CHUNK_V2_SECTION_STREAM:
        for (const auto& value :
             typedSection<D9CCommandChunkWireStreamBindingV2>(section)) {
          if (value.valid) {
            appendObject(hazards.reads, D9C_CHUNK_HANDLE_KIND_BUFFER,
                         record.objectForAbsoluteIndex(value.handleIndex));
          }
        }
        break;
      case D9C_COMMAND_CHUNK_V2_SECTION_INDEX_BUFFER: {
        const auto& value =
            typedSection<D9CCommandChunkWireIndexBindingV2>(section).front();
        if (value.valid) {
          appendObject(hazards.reads, D9C_CHUNK_HANDLE_KIND_BUFFER,
                       record.objectForAbsoluteIndex(value.handleIndex));
        }
        break;
      }
      case D9C_COMMAND_CHUNK_V2_SECTION_RENDER_TARGET:
        for (const auto& value :
             typedSection<D9CCommandChunkWireRenderTargetBindingV2>(section)) {
          if (value.valid) {
            appendObject(hazards.writes, D9C_CHUNK_HANDLE_KIND_SURFACE,
                         record.objectForAbsoluteIndex(value.handleIndex));
          }
        }
        break;
      case D9C_COMMAND_CHUNK_V2_SECTION_DEPTH_STENCIL: {
        const auto& value =
            typedSection<D9CCommandChunkWireDepthStencilBindingV2>(section)
                .front();
        if (value.valid) {
          appendObject(hazards.writes, D9C_CHUNK_HANDLE_KIND_SURFACE,
                       record.objectForAbsoluteIndex(value.handleIndex));
        }
        break;
      }
      default:
        break;
    }
  }
}

template <typename Fixed>
void collectFixedPair(const ResolvedRecordV2View& record,
                      std::uint32_t Fixed::* src,
                      std::uint32_t Fixed::* dst,
                      std::uint32_t srcKind,
                      std::uint32_t dstKind,
                      ImportedRecordResourceHazards& hazards) {
  Fixed fixed{};
  if (!load(record.wire.payload, fixed)) {
    return;
  }
  appendObject(hazards.reads, srcKind,
               record.objectForAbsoluteIndex(fixed.*src));
  appendObject(hazards.writes, dstKind,
               record.objectForAbsoluteIndex(fixed.*dst));
}

bool resolvedRecordValid(const ResolvedRecordV2View& record) {
  return v2RecordRule(record.wire.header.type) &&
         record.objects.size() == record.wire.header.handleCount &&
         std::none_of(record.objects.begin(), record.objects.end(),
                      [](const void* object) { return object == nullptr; });
}

}  // namespace

void collectImportedRecordResourceHazardsV2(
    const ResolvedRecordV2View& record,
    ImportedRecordResourceHazards& hazards) {
  if (!resolvedRecordValid(record)) {
    return;
  }
  switch (record.wire.header.type) {
    case D9C_COMMAND_RECORD_DRAW_PRIMITIVE:
    case D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE:
    case D9C_COMMAND_RECORD_DRAW_PRIMITIVE_UP:
    case D9C_COMMAND_RECORD_DRAW_INDEXED_PRIMITIVE_UP:
      collectSparseHazards(record, hazards);
      break;
    case D9C_COMMAND_RECORD_STRETCH_RECT:
      collectFixedPair(
          record, &D9CCommandChunkWireStretchRectV2::srcHandleIndex,
          &D9CCommandChunkWireStretchRectV2::dstHandleIndex,
          D9C_CHUNK_HANDLE_KIND_SURFACE, D9C_CHUNK_HANDLE_KIND_SURFACE,
          hazards);
      break;
    case D9C_COMMAND_RECORD_COLOR_FILL: {
      D9CCommandChunkWireColorFillV2 fixed{};
      if (load(record.wire.payload, fixed)) {
        appendObject(hazards.writes, D9C_CHUNK_HANDLE_KIND_SURFACE,
                     record.objectForAbsoluteIndex(fixed.surfaceHandleIndex));
      }
      break;
    }
    case D9C_COMMAND_RECORD_UPDATE_TEXTURE:
      collectFixedPair(
          record, &D9CCommandChunkWireUpdateTextureV2::srcHandleIndex,
          &D9CCommandChunkWireUpdateTextureV2::dstHandleIndex,
          D9C_CHUNK_HANDLE_KIND_TEXTURE, D9C_CHUNK_HANDLE_KIND_TEXTURE,
          hazards);
      break;
    case D9C_COMMAND_RECORD_UPDATE_SURFACE:
      collectFixedPair(
          record, &D9CCommandChunkWireUpdateSurfaceV2::srcHandleIndex,
          &D9CCommandChunkWireUpdateSurfaceV2::dstHandleIndex,
          D9C_CHUNK_HANDLE_KIND_SURFACE, D9C_CHUNK_HANDLE_KIND_SURFACE,
          hazards);
      break;
    case D9C_COMMAND_RECORD_READBACK:
      collectFixedPair(
          record, &D9CCommandChunkWireReadbackV2::srcHandleIndex,
          &D9CCommandChunkWireReadbackV2::dstHandleIndex,
          D9C_CHUNK_HANDLE_KIND_SURFACE, D9C_CHUNK_HANDLE_KIND_SURFACE,
          hazards);
      break;
    case D9C_COMMAND_RECORD_RESZ_DEPTH_RESOLVE:
      collectFixedPair(
          record,
          &D9CCommandChunkWireReszDepthResolveV2::msaaDepthHandleIndex,
          &D9CCommandChunkWireReszDepthResolveV2::intzDestHandleIndex,
          D9C_CHUNK_HANDLE_KIND_SURFACE, D9C_CHUNK_HANDLE_KIND_TEXTURE,
          hazards);
      break;
    default:
      break;
  }
}

bool v2RecordRequiresEffectiveResourceMarking(
    const ResolvedRecordV2View& record) noexcept {
  const auto* rule = v2RecordRule(record.wire.header.type);
  return rule && (rule->ruleFlags & V2RecordRuleDraw) != 0u;
}

ImportedReplayOrderingDecision evaluateImportedReplayOrderingV2(
    const ResolvedRecordV2View& record,
    const ImportedReplayHazardState& active) noexcept {
  ImportedReplayOrderingDecision decision{};
  decision.replayInfo =
      replayInfoForCommandRecordType(record.wire.header.type);
  if (!resolvedRecordValid(record)) {
    decision.action = ImportedReplayOrderingAction::InvalidRecord;
    decision.resetsActiveHazards = true;
    return decision;
  }

  collectImportedRecordResourceHazardsV2(record, decision.recordHazards);
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

}  // namespace dxmt9::d3d9::devicec
