#include "device_c_record_utils.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace dxmt9::d3d9::devicec {
namespace {

bool handleSetContains(
    const ImportedChunkHandleSet& handles,
    std::uint32_t kind,
    std::uint64_t handle) noexcept {
  if (handle == 0 || kind > D9C_CHUNK_HANDLE_KIND_QUERY) {
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
}  // namespace

bool appendImportedChunkHandle(
    ImportedChunkHandleSet& handles,
    std::uint32_t kind,
    std::uint64_t handle) {
  if (handle == 0 || kind > D9C_CHUNK_HANDLE_KIND_QUERY) {
    return false;
  }
  auto& bucket = handles.byKind[kind];
  if (std::find(bucket.begin(), bucket.end(), handle) != bucket.end()) {
    return false;
  }
  bucket.push_back(handle);
  return true;
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

}  // namespace dxmt9::d3d9::devicec
