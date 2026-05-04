#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

#include "dxmt9/core.hpp"
#include "dxmt9/device_c.h"

namespace dxmt9::d3d9::devicec {

enum class D9CCommandRecordValidationStatus : std::uint8_t {
  Valid,
  TruncatedHeader,
  SizeTooSmall,
  TruncatedRecord,
  UnknownType,
  SizeMismatch,
};

struct D9CCommandRecordValidation {
  D9CCommandRecordHeader header{};
  std::uint32_t availableBytes = 0;
  std::uint32_t minimumSize = sizeof(D9CCommandRecordHeader);
  std::uint64_t expectedSize = sizeof(D9CCommandRecordHeader);
  D9CCommandRecordValidationStatus status =
      D9CCommandRecordValidationStatus::TruncatedHeader;

  bool valid() const noexcept {
    return status == D9CCommandRecordValidationStatus::Valid;
  }
};

struct ImportedChunkView {
  const std::uint8_t* records = nullptr;
  std::uint32_t recordBytes = 0;
  std::uint32_t recordCount = 0;

  bool empty() const noexcept {
    return recordBytes == 0;
  }
};

struct ImportedRecordView {
  const std::uint8_t* record = nullptr;
  std::uint32_t offset = 0;
  std::uint32_t index = 0;
  D9CCommandRecordHeader header{};
  D9CCommandRecordValidation validation{};

  bool valid() const noexcept {
    return validation.valid();
  }

  std::uint32_t nextOffset() const noexcept {
    return offset + header.size;
  }

  std::uint32_t nextIndex() const noexcept {
    return index + 1u;
  }
};

enum class ImportedChunkValidationStatus : std::uint8_t {
  Valid,
  InvalidRecord,
  RecordCountMismatch,
};

struct ImportedChunkValidation {
  ImportedChunkView chunk{};
  ImportedChunkValidationStatus status =
      ImportedChunkValidationStatus::InvalidRecord;
  std::uint32_t consumedBytes = 0;
  std::uint32_t parsedRecordCount = 0;
  ImportedRecordView failedRecord{};

  bool valid() const noexcept {
    return status == ImportedChunkValidationStatus::Valid;
  }
};

enum class ImportedDrawRunScanStop : std::uint8_t {
  NotDrawRecord,
  FirstRecordHasStateDelta,
  EndOfChunk,
  InvalidRecord,
  DifferentRecordType,
  StateDelta,
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

struct ImportedChunkHandleSet {
  std::array<std::vector<std::uint64_t>, 5> byKind{};
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

struct ImportedDrawRunScan {
  ImportedRecordView firstRecord{};
  ImportedRecordView stopRecord{};
  std::uint32_t recordType = 0;
  std::uint32_t endOffset = 0;
  std::uint32_t endIndex = 0;
  std::uint32_t recordCount = 0;
  ImportedDrawRunScanStop stop = ImportedDrawRunScanStop::NotDrawRecord;

  bool replayAsRun() const noexcept {
    return recordCount >= 2u;
  }
};

D9CCommandRecordValidation validateCommandRecord(
    const std::uint8_t* record,
    std::uint32_t availableBytes) noexcept;

const char* commandRecordValidationStatusName(
    D9CCommandRecordValidationStatus status) noexcept;

ImportedChunkView makeImportedChunkView(
    const std::uint8_t* records,
    std::uint32_t recordBytes,
    std::uint32_t recordCount) noexcept;

ImportedRecordView makeImportedRecordView(
    const ImportedChunkView& chunk,
    std::uint32_t offset,
    std::uint32_t index) noexcept;

std::optional<ImportedRecordView> nextImportedRecord(
    const ImportedChunkView& chunk,
    std::uint32_t offset,
    std::uint32_t index) noexcept;

bool importedChunkRecordCountMatches(
    const ImportedChunkView& chunk,
    std::uint32_t actualRecordCount) noexcept;

ImportedChunkValidation validateImportedChunk(
    const ImportedChunkView& chunk) noexcept;

ImportedDrawRunScan scanImportedDrawRun(
    const ImportedChunkView& chunk,
    const ImportedRecordView& firstRecord) noexcept;

ImportedRecordReplayInfo replayInfoForCommandRecordType(std::uint32_t type) noexcept;
ImportedRecordReplayInfo replayInfoForImportedRecord(const ImportedRecordView& record) noexcept;

bool appendImportedChunkHandle(
    ImportedChunkHandleSet& handles,
    std::uint32_t kind,
    std::uint64_t handle);

void collectDrawPacketResourceHandles(
    const D9CDrawPrimitivePacket& packet,
    ImportedChunkHandleSet& handles);

void collectIndexedDrawPacketResourceHandles(
    const D9CDrawIndexedPrimitivePacket& packet,
    ImportedChunkHandleSet& handles);

void collectImportedRecordResourceHandles(
    const ImportedRecordView& record,
    ImportedChunkHandleSet& handles);

void collectImportedRecordResourceHazards(
    const ImportedRecordView& record,
    ImportedRecordResourceHazards& hazards);

bool importedRecordHazardsOverlap(
    const ImportedReplayHazardState& active,
    const ImportedRecordResourceHazards& record,
    bool* readAfterWrite = nullptr,
    bool* writeAfterRead = nullptr,
    bool* writeAfterWrite = nullptr) noexcept;

ImportedReplayOrderingDecision evaluateImportedReplayOrdering(
    const ImportedRecordView& record,
    const ImportedReplayHazardState& active) noexcept;

ImportedReplayHazardState nextImportedReplayHazardState(
    const ImportedReplayHazardState& active,
    const ImportedReplayOrderingDecision& decision);

std::vector<D9CChunkHandleEntry> makeImportedChunkHandleEntries(
    const ImportedChunkHandleSet& handles);

bool packetHasNoStateDelta(const D9CDrawPrimitivePacket& packet) noexcept;
dxmt9::core::DrawParam makeRunParam(const D9CDrawPrimitivePacket& packet) noexcept;
dxmt9::core::DrawParam makeRunParam(
    const D9CDrawIndexedPrimitivePacket& packet) noexcept;

}  // namespace dxmt9::d3d9::devicec
