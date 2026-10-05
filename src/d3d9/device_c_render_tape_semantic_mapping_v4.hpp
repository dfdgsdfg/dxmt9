#pragma once

#include "device_c_render_tape_source_plan_v4.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace dxmt9::d3d9 {

enum class RenderTapeSemanticMappingV4Status : std::uint8_t {
  Valid, InvalidPlan, InvalidFacts, InvalidCandidate, SemanticMismatch,
  InvalidPrefix, UnsupportedInitializer, UnsupportedCoordinatorExpansion,
  CapacityExceeded, AllocationFailed,
};

struct RenderTapeSemanticMappingV4Limits {
  std::uint32_t maxEvents = 65536u;
  std::uint32_t maxRecords = 65536u;
  std::uint32_t maxSources = 4096u;
  std::uint32_t maxCommands = 65536u;
  std::uint32_t maxDispositions = 65536u;
  std::uint32_t maxCoordinators = 4096u;
  std::uint32_t maxRaws = 4096u;
  std::uint32_t maxAttempts = 65536u;
  std::uint32_t maxCommandRelations = 131072u;
  std::uint32_t maxSourceRelations = 131072u;
};

// Original locations remain immutable. A coordinator owns a canonical record,
// but may own several physical commands; its record has no single command.
struct RenderTapeSemanticMappingV4Record {
  std::uint64_t eventOrdinal = 0u;
  std::uint64_t capturedRawOrdinal = 0u;
  std::uint64_t candidateRawOrdinal = 0u;
  std::uint32_t recordIndex = 0u;
  std::uint32_t recordType = 0u;
  std::uint32_t dispositionKind = 0u;
  std::uint32_t capturedDispositionIndex = kRenderTapeIdentityV4NoIndex;
  std::uint32_t candidateDispositionIndex = kRenderTapeIdentityV4NoIndex;
  std::uint32_t capturedSourceIndex = kRenderTapeIdentityV4NoIndex;
  std::uint32_t candidateSourceIndex = kRenderTapeIdentityV4NoIndex;
  std::uint32_t capturedCommandIndex = kRenderTapeIdentityV4NoIndex;
  std::uint32_t candidateCommandIndex = kRenderTapeIdentityV4NoIndex;
  std::uint32_t capturedPhysicalCommandIndex = kRenderTapeIdentityV4NoIndex;
  std::uint32_t candidatePhysicalCommandIndex = kRenderTapeIdentityV4NoIndex;
  std::uint32_t capturedSubdraw = kRenderTapeIdentityV4NoIndex;
  std::uint32_t candidateSubdraw = kRenderTapeIdentityV4NoIndex;
  std::uint32_t capturedCoordinatorIndex = kRenderTapeIdentityV4NoIndex;
  std::uint32_t candidateCoordinatorIndex = kRenderTapeIdentityV4NoIndex;
  friend constexpr bool operator==(const RenderTapeSemanticMappingV4Record&,
      const RenderTapeSemanticMappingV4Record&) = default;
};

// Many-to-many relations contain actual contributing origins, never a product
// of source extents. Coordinator-generated commands have recordCount == 0.
struct RenderTapeSemanticMappingV4Command {
  std::uint32_t capturedCommandIndex = kRenderTapeIdentityV4NoIndex;
  std::uint32_t candidateCommandIndex = kRenderTapeIdentityV4NoIndex;
  std::uint32_t capturedCoordinatorIndex = kRenderTapeIdentityV4NoIndex;
  std::uint32_t candidateCoordinatorIndex = kRenderTapeIdentityV4NoIndex;
  std::uint32_t recordCount = 0u;
  friend constexpr bool operator==(const RenderTapeSemanticMappingV4Command&,
      const RenderTapeSemanticMappingV4Command&) = default;
};

struct RenderTapeSemanticMappingV4Source {
  std::uint32_t capturedIndex = kRenderTapeIdentityV4NoIndex;
  std::uint32_t candidateIndex = kRenderTapeIdentityV4NoIndex;
  std::uint32_t recordCount = 0u;
  std::uint32_t coordinatorCommandCount = 0u;
  std::uint64_t candidateSequence = 0u;
  std::uint64_t candidatePublicationEdge = 0u;
  friend constexpr bool operator==(const RenderTapeSemanticMappingV4Source&,
      const RenderTapeSemanticMappingV4Source&) = default;
};

struct RenderTapeSemanticMappingV4Raw {
  std::uint64_t eventOrdinal = 0u;
  std::uint64_t capturedRawOrdinal = 0u;
  std::uint64_t candidateRawOrdinal = 0u;
  std::uint64_t candidateReplayEdge = 0u;
  std::uint32_t capturedSettlementIndex = kRenderTapeIdentityV4NoIndex;
  std::uint32_t candidateSettlementIndex = kRenderTapeIdentityV4NoIndex;
  std::uint32_t recordCount = 0u;
  std::uint32_t committedAttemptCount = 0u;
  std::uint32_t rolledBackAttemptCount = 0u;
  friend constexpr bool operator==(const RenderTapeSemanticMappingV4Raw&,
      const RenderTapeSemanticMappingV4Raw&) = default;
};

struct RenderTapeSemanticMappingV4Coordinator {
  std::uint32_t capturedIndex = kRenderTapeIdentityV4NoIndex;
  std::uint32_t candidateIndex = kRenderTapeIdentityV4NoIndex;
  std::uint32_t candidateSourceIndex = kRenderTapeIdentityV4NoIndex;
  std::uint32_t candidateInitializerIndex = kRenderTapeIdentityV4NoIndex;
  std::uint32_t candidateCommandEnd = 0u;
  std::uint32_t candidatePrefixKind = 0u;
  std::uint32_t prefixRecordCount = 0u;
  std::uint32_t prefixCoordinatorCommandCount = 0u;
  std::uint64_t candidatePrefixSequence = 0u;
  std::uint64_t candidatePrefixInitializer = 0u;
  friend constexpr bool operator==(const RenderTapeSemanticMappingV4Coordinator&,
      const RenderTapeSemanticMappingV4Coordinator&) = default;
};

struct RenderTapeSemanticMappingV4Obligation {
  std::uint32_t capturedIndex = kRenderTapeIdentityV4NoIndex;
  std::uint32_t firstSourceRelation = 0u;
  std::uint32_t sourceRelationCount = 0u;
  friend constexpr bool operator==(const RenderTapeSemanticMappingV4Obligation&,
      const RenderTapeSemanticMappingV4Obligation&) = default;
};

// Cold owned correspondence only. Valid proves canonical semantic coverage and
// genuine complete provenance; it grants no replay, completion, readiness,
// topology, pixels or timing authority. No input span survives the build.
class RenderTapeSemanticMappingV4 {
public:
  RenderTapeSemanticMappingV4() = default;
  RenderTapeSemanticMappingV4(const RenderTapeSemanticMappingV4&) = delete;
  RenderTapeSemanticMappingV4& operator=(const RenderTapeSemanticMappingV4&) = delete;
  RenderTapeSemanticMappingV4(RenderTapeSemanticMappingV4&&) noexcept;
  RenderTapeSemanticMappingV4& operator=(RenderTapeSemanticMappingV4&&) noexcept;

  bool valid() const noexcept { return status_ == RenderTapeSemanticMappingV4Status::Valid; }
  RenderTapeSemanticMappingV4Status status() const noexcept { return status_; }
  const RenderTapeDigest& planDigest() const noexcept { return planDigest_; }
  RenderTapeIdentityV4CaptureKey candidateCapture() const noexcept { return candidateCapture_; }
  std::uint64_t candidateQueue() const noexcept { return candidateQueue_; }
  std::span<const RenderTapeSemanticMappingV4Record> records() const noexcept { return records_; }
  std::span<const RenderTapeSemanticMappingV4Command> commands() const noexcept { return commands_; }
  std::span<const RenderTapeSemanticMappingV4Source> sources() const noexcept { return sources_; }
  std::span<const RenderTapeSemanticMappingV4Raw> raws() const noexcept { return raws_; }
  std::span<const RenderTapeSemanticMappingV4Coordinator> coordinators() const noexcept {
    return coordinators_;
  }
  std::span<const RenderTapeSemanticMappingV4Obligation> obligations() const noexcept {
    return obligations_;
  }

private:
  friend RenderTapeSemanticMappingV4Status buildRenderTapeSemanticMappingV4(
      const CaptureSourceReplayPlanV4&, const RenderTapeIdentityV4View&,
      std::span<const RenderTapeIdentityV3EventFact>,
      std::span<const RenderTapeIdentityV3RecordFact>,
      std::span<const CaptureSourceReplayPlanV4Action>,
      RenderTapeSemanticMappingV4Limits, RenderTapeSemanticMappingV4&) noexcept;
  RenderTapeSemanticMappingV4Status status_ = RenderTapeSemanticMappingV4Status::InvalidPlan;
  RenderTapeDigest planDigest_{};
  RenderTapeIdentityV4CaptureKey candidateCapture_{};
  std::uint64_t candidateQueue_ = 0u;
  std::vector<RenderTapeSemanticMappingV4Record> records_{};
  std::vector<RenderTapeSemanticMappingV4Command> commands_{};
  std::vector<RenderTapeSemanticMappingV4Source> sources_{};
  std::vector<RenderTapeSemanticMappingV4Raw> raws_{};
  std::vector<RenderTapeSemanticMappingV4Coordinator> coordinators_{};
  std::vector<RenderTapeSemanticMappingV4Obligation> obligations_{};
};

// canonicalEvents must match the admitted tape; localRecords preserve every
// canonical field except the real local query identity/cycle/issue ordinal.
// Candidate rows are independently fully revalidated before mapping. Original
// reference publication cuts are obligations qualified by final coverage, not
// requests to flush. Every failure invalidates out and exposes no partial map.
// This first bounded slice rejects nonempty interval initializer tables after
// the general guards. Genuine arm baseline dependencies remain distinct.
[[nodiscard]] RenderTapeSemanticMappingV4Status buildRenderTapeSemanticMappingV4(
    const CaptureSourceReplayPlanV4& plan,
    const RenderTapeIdentityV4View& candidate,
    std::span<const RenderTapeIdentityV3EventFact> canonicalEvents,
    std::span<const RenderTapeIdentityV3RecordFact> localRecords,
    std::span<const CaptureSourceReplayPlanV4Action> sourceObligations,
    RenderTapeSemanticMappingV4Limits limits,
    RenderTapeSemanticMappingV4& out) noexcept;

const char* renderTapeSemanticMappingV4StatusName(RenderTapeSemanticMappingV4Status) noexcept;

} // namespace dxmt9::d3d9
