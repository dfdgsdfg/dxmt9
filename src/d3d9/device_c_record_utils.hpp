#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

#include "dxmt9/core.hpp"
#include "dxmt9/device_c.h"

namespace dxmt9::d3d9 {
struct ResolvedRecordV2View;
}

namespace dxmt9::d3d9::devicec {

struct ImportedChunkHandleSet {
  std::array<std::vector<std::uint64_t>, 6> byKind{};
};

struct ImportedRecordResourceHazards {
  ImportedChunkHandleSet reads{};
  ImportedChunkHandleSet writes{};
};

struct ImportedReplayHazardState {
  ImportedRecordResourceHazards hazards{};
  bool active = false;
};

enum class ImportedReplayOrderingAction : std::uint8_t {
  InvalidRecord,
  Continue,
  BarrierBoundary,
  HazardBoundary,
  SynchronousReadBoundary,
};

enum class ImportedRecordReplayCategory : std::uint8_t {
  Unknown,
  Draw,
  ConstantUpload,
  StateApply,
  Clear,
  Present,
  SurfaceOp,
  QueryIssue,
  Readback,
};

struct ImportedRecordReplayInfo {
  ImportedRecordReplayCategory category = ImportedRecordReplayCategory::Unknown;
  bool ordered = false;
  bool mutatesDeviceState = false;
  bool readsDeviceState = false;
  bool referencesResources = false;
  bool draw = false;
  bool barrier = false;
  bool synchronousReadBoundary = false;
};

struct ImportedReplayOrderingDecision {
  ImportedReplayOrderingAction action = ImportedReplayOrderingAction::InvalidRecord;
  ImportedRecordReplayInfo replayInfo{};
  ImportedRecordResourceHazards recordHazards{};
  bool readAfterWrite = false;
  bool writeAfterRead = false;
  bool writeAfterWrite = false;
  bool resetsActiveHazards = false;

  bool hazardBoundary() const noexcept {
    return action == ImportedReplayOrderingAction::HazardBoundary;
  }

  bool barrierBoundary() const noexcept {
    return action == ImportedReplayOrderingAction::BarrierBoundary ||
           action == ImportedReplayOrderingAction::SynchronousReadBoundary;
  }
};

// The only surviving legacy-record helper: it maps a record TYPE to replay
// properties, keyed on the type tag alone, so the V2 path uses it too --
// device_c_chunk_v2_hazard.cpp and the V2 chunk replay both call it.
ImportedRecordReplayInfo replayInfoForCommandRecordType(std::uint32_t type) noexcept;

bool appendImportedChunkHandle(
    ImportedChunkHandleSet& handles,
    std::uint32_t kind,
    std::uint64_t handle);

bool importedRecordHazardsOverlap(
    const ImportedReplayHazardState& active,
    const ImportedRecordResourceHazards& record,
    bool* readAfterWrite = nullptr,
    bool* writeAfterRead = nullptr,
    bool* writeAfterWrite = nullptr) noexcept;

ImportedReplayHazardState nextImportedReplayHazardState(
    const ImportedReplayHazardState& active,
    const ImportedReplayOrderingDecision& decision);

void collectImportedRecordResourceHazardsV2(
    const ResolvedRecordV2View& record,
    ImportedRecordResourceHazards& hazards);

bool v2RecordRequiresEffectiveResourceMarking(
    const ResolvedRecordV2View& record) noexcept;

ImportedReplayOrderingDecision evaluateImportedReplayOrderingV2(
    const ResolvedRecordV2View& record,
    const ImportedReplayHazardState& active) noexcept;

}  // namespace dxmt9::d3d9::devicec
