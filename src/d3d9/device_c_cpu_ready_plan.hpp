#pragma once

#include "device_c_chunk_validate.hpp"
#include "device_c_replay_offload.hpp"
#include "../dxmt9/dxmt9_source_payload.hpp"

#include <cstddef>
#include <cstdint>
#include <array>
#include <limits>
#include <optional>
#include <vector>

namespace dxmt9::d3d9 {

struct CpuReadyPlanOptions {
  std::size_t pageSize = 4096;
  std::size_t maxOrdinaryPagesPerSegment = 64;
  std::size_t maxSegmentsPerSource =
      core::kMaxArenaSourcePayloadSegments;
  std::size_t maxPagesPerSource =
      std::numeric_limits<std::uint32_t>::max();
  // Compatibility alias used by the current single-block production caller.
  // The effective source bound is min(maxPages, maxPagesPerSource).
  std::size_t maxPages = std::numeric_limits<std::uint32_t>::max();
  // Compatibility default keeps the current one-logical-source contract.
  // Callers opting into source segmentation may group the bounded physical
  // segments into at most this many source plans.
  std::size_t maxSourcesPerChunk = 1;
};

// Future arena construction appends every UP byte range and synthesized
// binding payload independently with this alignment. Planning applies the
// same policy with checked padding, so drawPayloadBytes is a safe upper bound.
inline constexpr std::size_t kArenaDrawPayloadAppendAlignment =
    core::kSourcePayloadByteAlignment;

struct CpuReadySegmentPlan {
  // Exact semantic-replay range for this construction segment. Adjacent
  // segments partition the complete raw record stream without gaps: leading
  // and interstitial state belongs to the following GPU-producing record,
  // while trailing state belongs to the final segment.
  std::uint32_t firstRecordIndex = 0;
  std::uint32_t recordCount = 0;
  bool jumbo = false;
  core::SourcePayloadCapacity capacity{};
  core::SourcePayloadLayout layout{};
};

struct CpuReadySourcePlan {
  // A source plan borrows contiguous entries from CpuReadyPlan::segments;
  // it owns no storage or payload pointers. Record ranges remain raw-chunk
  // relative and therefore preserve state-only prefix/interstitial/tail
  // ownership exactly when a source boundary is introduced.
  std::uint32_t firstSegmentIndex = 0;
  std::uint32_t segmentCount = 0;
  std::uint32_t firstRecordIndex = 0;
  std::uint32_t recordCount = 0;
  bool jumbo = false;
  core::ArenaSourcePayloadLayout arenaLayout{};

  bool valid() const noexcept {
    return segmentCount != 0 && recordCount != 0 &&
           arenaLayout.segmentCount == segmentCount &&
           arenaLayout.valid();
  }
};

struct CpuReadyPlan {
  RawOrdinal rawOrdinal = 0;
  ReplayLane lane = ReplayLane::Legacy;
  ReplayReason reason = ReplayReason::InvalidImportedView;
  bool logicalSource = false;
  core::SourcePayloadCapacity capacity{};
  // Physical segments are bounded by the producer's raw-record cap, not by
  // the per-source Arena-block cap. Keep the vectors off the stack; planning
  // catches reserve failure and returns a pre-replay rejection.
  std::vector<CpuReadySegmentPlan> segments{};
  std::size_t segmentCount = 0;
  std::vector<CpuReadySourcePlan> sources{};
  std::size_t sourceCount = 0;
  std::optional<core::ArenaSourcePayloadLayout> arenaLayout{};
  // True when the complete raw stream contains at least one structurally
  // validated Query, Readback, or UpdateTexture. The plan intentionally does
  // not retain a variable-size disposition list: compatibility replay rebuilds
  // each allocation-free descriptor at its exact record index after this
  // whole-raw preflight has succeeded.
  bool containsOrderedControls = false;
  // Temporary compatibility surface for the existing production replay
  // connection. Multi-segment plans intentionally leave this empty; P3 moves
  // production routing to arenaLayout and the segment table.
  std::optional<core::SourcePayloadLayout> layout{};

  bool directArenaCandidate() const noexcept {
    return lane == ReplayLane::DirectArenaCandidate && logicalSource &&
           sourceCount != 0;
  }

  bool requiresAdmission() const noexcept { return directArenaCandidate(); }

  // Every non-rejected whole-raw disposition must replay D3D semantics once;
  // StateOnly is explicit because it performs replay without source admission.
  bool replaysSemanticsExactlyOnce() const noexcept {
    return lane != ReplayLane::Reject;
  }
};

// Scans one already-validated ImportedChunkView structurally. It does not
// resolve handles, mutate D3D state, invoke semantic replay, or reserve Tape
// storage. The returned lane covers the whole raw chunk.
CpuReadyPlan planCpuReadyChunk(
    const ImportedChunkView& imported,
    RawOrdinal rawOrdinal,
    CpuReadyPlanOptions options = {}) noexcept;

}  // namespace dxmt9::d3d9
