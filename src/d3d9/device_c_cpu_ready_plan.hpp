#pragma once

#include "device_c_chunk_v2_validate.hpp"
#include "device_c_replay_offload.hpp"
#include "../dxmt9/dxmt9_source_payload.hpp"

#include <cstddef>
#include <cstdint>
#include <array>
#include <limits>
#include <optional>

namespace dxmt9::d3d9 {

struct V2CpuReadyPlanOptions {
  std::size_t pageSize = 4096;
  std::size_t maxOrdinaryPagesPerSegment = 64;
  std::size_t maxSegmentsPerSource =
      core::kMaxArenaSourcePayloadSegments;
  std::size_t maxPagesPerSource =
      std::numeric_limits<std::uint32_t>::max();
  // Compatibility alias used by the current single-block production caller.
  // The effective source bound is min(maxPages, maxPagesPerSource).
  std::size_t maxPages = std::numeric_limits<std::uint32_t>::max();
};

// Future arena construction appends every UP byte range and synthesized
// binding payload independently with this alignment. Planning applies the
// same policy with checked padding, so drawPayloadBytes is a safe upper bound.
inline constexpr std::size_t kV2ArenaDrawPayloadAppendAlignment =
    core::kSourcePayloadByteAlignment;

struct V2CpuReadySegmentPlan {
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

struct V2CpuReadyPlan {
  RawOrdinal rawOrdinal = 0;
  V2ReplayLane lane = V2ReplayLane::Legacy;
  V2ReplayReason reason = V2ReplayReason::InvalidImportedView;
  bool logicalSource = false;
  core::SourcePayloadCapacity capacity{};
  std::array<V2CpuReadySegmentPlan,
             core::kMaxArenaSourcePayloadSegments> segments{};
  std::size_t segmentCount = 0;
  std::optional<core::ArenaSourcePayloadLayout> arenaLayout{};
  // Temporary compatibility surface for the existing production replay
  // connection. Multi-segment plans intentionally leave this empty; P3 moves
  // production routing to arenaLayout and the segment table.
  std::optional<core::SourcePayloadLayout> layout{};

  bool directArenaCandidate() const noexcept {
    return lane == V2ReplayLane::DirectArenaCandidate && logicalSource &&
           arenaLayout.has_value();
  }

  bool requiresAdmission() const noexcept { return directArenaCandidate(); }

  // Every non-rejected whole-raw disposition must replay D3D semantics once;
  // StateOnly is explicit because it performs replay without source admission.
  bool replaysSemanticsExactlyOnce() const noexcept {
    return lane != V2ReplayLane::Reject;
  }
};

// Scans one already-validated ImportedChunkV2View structurally. It does not
// resolve handles, mutate D3D state, invoke semantic replay, or reserve Tape
// storage. The returned lane covers the whole raw chunk.
V2CpuReadyPlan planCpuReadyChunkV2(
    const ImportedChunkV2View& imported,
    RawOrdinal rawOrdinal,
    V2CpuReadyPlanOptions options = {}) noexcept;

}  // namespace dxmt9::d3d9
