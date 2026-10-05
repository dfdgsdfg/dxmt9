#pragma once

#include "device_c_render_tape_identity_v3.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace dxmt9::d3d9 {

enum class TapeCoordinatorAuthority : std::uint32_t {
  SessionRelease = 1u,
  OrdinarySerial = 2u,
};

struct TapeCoordinatorAdmissionEvidence {
  TapeCoordinatorAuthority authority = TapeCoordinatorAuthority::SessionRelease;
  std::uint64_t admissionEdge = 0u;
};

// Shared captured provenance only. No serialized version, Pass membership,
// selected Encode plan or GPU execution authority is supplied by this context.
struct TapeProvenanceContext {
  std::uint64_t captureToken = 0u;
  std::uint64_t captureEpoch = 0u;
  std::uint64_t queue = 0u;
  std::uint64_t baselineSequence = 0u;
  std::uint64_t baselineInitializer = 0u;
  std::uint64_t baselineCompletedSequence = 0u;
  std::uint64_t initializerEpoch = 0u;
  std::uint64_t baselineCompletedInitializer = 0u;
  std::uint64_t baselineCompletionEdge = 0u;
  std::uint32_t baselineKind = 0u;
  TapeCoordinatorAuthority coordinatorDomain = TapeCoordinatorAuthority::SessionRelease;
};

enum class TapeProvenanceStatus : std::uint8_t {
  Valid, InvalidTape, InvalidHeader, InvalidSource, InvalidCut,
  RecordCoverageMismatch, InvalidDisposition, InvalidCoordinator,
  InvalidPrefix, InvalidSettlement, Incomplete, CapacityExceeded,
  AllocationFailed,
};

// A physical extent projection, never a synthetic V3 command or Pass row.
struct TapePhysicalCommand {
  std::uint32_t sourceIndex = kRenderTapeIdentityV3NoIndex;
  std::uint32_t physicalCommandIndex = 0u;
  std::uint32_t kind = 0u;
  std::uint32_t drawCount = 0u;
  std::uint32_t coordinatorIndex = kRenderTapeIdentityV3NoIndex;
};

// Cold borrowed adapter. Version-specific fields are checked at the original
// command traversal position; common validation owns the physical grammar.
struct TapePhysicalCommandTable {
  const void* rows = nullptr;
  std::size_t count = 0u;
  TapePhysicalCommand (*physicalAt)(const void*, std::size_t) noexcept = nullptr;
  bool (*admitVersionFields)(const void*, std::size_t) noexcept = nullptr;
  std::size_t size() const noexcept { return count; }
  TapePhysicalCommand operator[](std::size_t index) const noexcept {
    return physicalAt(rows, index);
  }
  bool versionFieldsAdmitted(std::size_t index) const noexcept {
    return admitVersionFields(rows, index);
  }
  bool valid() const noexcept {
    return !count || (rows && physicalAt && admitVersionFields);
  }
};

// Borrowed lifecycle projection: V4 publication metadata is retained in its
// own rows and never copied into fabricated V3 wire sources.
struct TapeProvenanceSourceTable {
  using At = RenderTapeIdentityV3Source (*)(const void*, std::size_t) noexcept;
  using Admit = bool (*)(const void*, std::size_t) noexcept;
  const void* rows = nullptr;
  std::size_t count = 0u;
  At sourceAt = nullptr;
  Admit admitVersionFields = nullptr;
  TapeProvenanceSourceTable() noexcept = default;
  TapeProvenanceSourceTable(const void* rows, std::size_t count, At at, Admit admit) noexcept
      : rows(rows), count(count), sourceAt(at), admitVersionFields(admit) {}
  TapeProvenanceSourceTable(std::span<const RenderTapeIdentityV3Source> values) noexcept
      : TapeProvenanceSourceTable(values.data(), values.size(), legacyAt, legacyAdmit) {}
  TapeProvenanceSourceTable(const std::vector<RenderTapeIdentityV3Source>& values) noexcept
      : TapeProvenanceSourceTable(std::span<const RenderTapeIdentityV3Source>(values)) {}
  std::size_t size() const noexcept { return count; }
  RenderTapeIdentityV3Source operator[](std::size_t index) const noexcept {
    return sourceAt(rows, index);
  }
  bool versionFieldsAdmitted(std::size_t index) const noexcept {
    return admitVersionFields(rows, index);
  }
  bool valid() const noexcept { return !count || (rows && sourceAt && admitVersionFields); }
private:
  static RenderTapeIdentityV3Source legacyAt(const void* rows, std::size_t index) noexcept {
    return static_cast<const RenderTapeIdentityV3Source*>(rows)[index];
  }
  static bool legacyAdmit(const void*, std::size_t) noexcept { return true; }
};

struct TapeRawSettlement {
  std::uint64_t eventOrdinal = 0u;
  std::uint64_t rawOrdinal = 0u;
  std::uint64_t replayEdge = 0u;
  std::uint32_t recordCount = 0u;
};

// The raw ledger join is shared; transaction authority is version-specific.
// V3 requires one actual generation/raw. V4 supplies its independently
// validated containing-attempt join, never a synthetic V3 settlement.
struct TapeRawSettlementTable {
  using At = TapeRawSettlement (*)(const void*, std::size_t) noexcept;
  using Admit = bool (*)(const void*, std::size_t) noexcept;
  using CoordinatorAdmit = bool (*)(const void*, std::size_t,
      const RenderTapeIdentityV3Coordinator&) noexcept;
  const void* rows = nullptr;
  std::size_t count = 0u;
  At settlementAt = nullptr;
  Admit admitVersionFields = nullptr;
  CoordinatorAdmit admitCoordinator = nullptr;
  TapeRawSettlementTable() noexcept = default;
  TapeRawSettlementTable(const void* rows, std::size_t count, At at, Admit admit,
      CoordinatorAdmit coordinatorAdmit) noexcept
      : rows(rows), count(count), settlementAt(at), admitVersionFields(admit),
        admitCoordinator(coordinatorAdmit) {}
  TapeRawSettlementTable(std::span<const RenderTapeIdentityV3ReplaySettlement> values) noexcept
      : TapeRawSettlementTable(values.data(), values.size(), legacyAt,
          legacyAdmit, legacyCoordinatorAdmit) {}
  TapeRawSettlementTable(const std::vector<RenderTapeIdentityV3ReplaySettlement>& values) noexcept
      : TapeRawSettlementTable(std::span<const RenderTapeIdentityV3ReplaySettlement>(values)) {}
  std::size_t size() const noexcept { return count; }
  TapeRawSettlement operator[](std::size_t index) const noexcept {
    return settlementAt(rows, index);
  }
  bool versionFieldsAdmitted(std::size_t index) const noexcept {
    return admitVersionFields(rows, index);
  }
  bool coordinatorAdmitted(std::size_t index,
      const RenderTapeIdentityV3Coordinator& row) const noexcept {
    return admitCoordinator(rows, index, row);
  }
  bool valid() const noexcept {
    return !count || (rows && settlementAt && admitVersionFields && admitCoordinator);
  }
private:
  static TapeRawSettlement legacyAt(const void* rows, std::size_t index) noexcept {
    const auto& row = static_cast<const RenderTapeIdentityV3ReplaySettlement*>(rows)[index];
    return {row.eventOrdinal, row.rawOrdinal, row.replayEdge, row.recordCount};
  }
  static bool legacyAdmit(const void* rows, std::size_t index) noexcept {
    const auto& row = static_cast<const RenderTapeIdentityV3ReplaySettlement*>(rows)[index];
    return row.buildGeneration && !row.reserved0;
  }
  static bool legacyCoordinatorAdmit(const void* rows, std::size_t index,
      const RenderTapeIdentityV3Coordinator& control) noexcept {
    return control.buildGeneration ==
        static_cast<const RenderTapeIdentityV3ReplaySettlement*>(rows)[index].buildGeneration;
  }
};

// Borrow the version's actual canonical row; V4's authority is projected
// explicitly, never inferred by converting it into a legacy wire view.
struct TapeCoordinatorTable {
  using At = const RenderTapeIdentityV3Coordinator& (*)(const void*, std::size_t) noexcept;
  using AdmissionAt = TapeCoordinatorAdmissionEvidence (*)(const void*, std::size_t) noexcept;
  const void* rows = nullptr;
  std::size_t count = 0u;
  At coordinatorAt = nullptr;
  AdmissionAt admissionAt = nullptr;
  TapeCoordinatorTable() noexcept = default;
  TapeCoordinatorTable(const void* rows, std::size_t count, At at, AdmissionAt admission) noexcept
      : rows(rows), count(count), coordinatorAt(at), admissionAt(admission) {}
  TapeCoordinatorTable(std::span<const RenderTapeIdentityV3Coordinator> values) noexcept
      : TapeCoordinatorTable(values.data(), values.size(), legacyAt, legacyAdmission) {}
  TapeCoordinatorTable(const std::vector<RenderTapeIdentityV3Coordinator>& values) noexcept
      : TapeCoordinatorTable(std::span<const RenderTapeIdentityV3Coordinator>(values)) {}
  std::size_t size() const noexcept { return count; }
  const RenderTapeIdentityV3Coordinator& operator[](std::size_t index) const noexcept {
    return coordinatorAt(rows, index);
  }
  TapeCoordinatorAdmissionEvidence admission(std::size_t index) const noexcept {
    return admissionAt(rows, index);
  }
  bool valid() const noexcept { return !count || (rows && coordinatorAt && admissionAt); }
  struct Iterator {
    const TapeCoordinatorTable* table;
    std::size_t index;
    const RenderTapeIdentityV3Coordinator& operator*() const noexcept { return (*table)[index]; }
    Iterator& operator++() noexcept { ++index; return *this; }
    bool operator!=(const Iterator& other) const noexcept { return index != other.index; }
  };
  Iterator begin() const noexcept { return {this, 0u}; }
  Iterator end() const noexcept { return {this, count}; }
private:
  static const RenderTapeIdentityV3Coordinator& legacyAt(const void* rows, std::size_t index) noexcept {
    return static_cast<const RenderTapeIdentityV3Coordinator*>(rows)[index];
  }
  static TapeCoordinatorAdmissionEvidence legacyAdmission(const void*, std::size_t) noexcept { return {}; }
};

struct TapeProvenanceRows {
  TapeProvenanceSourceTable sources{};
  std::span<const RenderTapeIdentityV3Initializer> initializers{};
  TapePhysicalCommandTable commands{};
  std::span<const RenderTapeIdentityV3Disposition> dispositions{};
  TapeCoordinatorTable coordinators{};
  TapeRawSettlementTable settlements{};
  std::span<const std::uint64_t> transactionEdges{};
};

bool tapeProvenanceContextAdmitted(const TapeProvenanceContext&) noexcept;
bool tapeProvenanceCommandShapeAdmitted(const TapePhysicalCommand&,
    std::span<const RenderTapeIdentityV3Source>,
    std::span<const RenderTapeIdentityV3Coordinator>) noexcept;
bool tapeProvenanceCoordinatorShapeAdmitted(const TapeProvenanceContext&,
    const RenderTapeIdentityV3Coordinator&,
    std::span<const RenderTapeIdentityV3Source>,
    std::span<const RenderTapeIdentityV3Initializer>,
    TapeCoordinatorAdmissionEvidence admission = {}) noexcept;
bool tapeProvenanceObservationAdmitted(const TapeProvenanceContext&,
    const RenderTapeIdentityV3Coordinator&, const RenderTapeIdentityV3CurrentIssue&,
    std::span<const RenderTapeIdentityV3Source>,
    std::span<const RenderTapeIdentityV3Initializer>,
    RenderTapeIdentityV3Observation,
    TapeCoordinatorAdmissionEvidence admission = {}) noexcept;

// One production tape/chunk/query-descriptor grammar for every identity
// version. Failure leaves both outputs unchanged; growth is budgeted first.
// Direct GetData controls retain full tape grammar validation, but these facts
// do not qualify external poll readiness against a live retained issue receipt.
TapeProvenanceStatus buildTapeCanonicalFacts(std::span<const std::byte> tape,
    const RenderTapeBlobCatalogue&, std::uint64_t presentOrdinal,
    std::uint32_t maxRows, std::vector<RenderTapeIdentityV3EventFact>& events,
    std::vector<RenderTapeIdentityV3RecordFact>& records) noexcept;

// Conserves all original records, physical commands, coordinator releases,
// immutable END cursors and independent render/initializer lifecycle edges.
// Valid remains provenance only: execution topology and transcript/live-receipt
// joins are separate qualification gates, including checkpoint poll-only tapes.
TapeProvenanceStatus validateTapeProvenanceRows(const TapeProvenanceContext&,
    const TapeProvenanceRows&, std::span<const RenderTapeIdentityV3EventFact>,
    std::span<const RenderTapeIdentityV3RecordFact>, std::uint32_t maxRows,
    bool requireComplete = true) noexcept;

} // namespace dxmt9::d3d9
