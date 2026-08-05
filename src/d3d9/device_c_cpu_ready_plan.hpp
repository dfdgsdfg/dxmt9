#pragma once

#include "device_c_chunk_v2_validate.hpp"
#include "device_c_replay_offload.hpp"
#include "../dxmt9/dxmt9_source_payload.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>

namespace dxmt9::d3d9 {

struct V2CpuReadyPlanOptions {
  std::size_t pageSize = 4096;
  std::size_t maxPages = std::numeric_limits<std::uint32_t>::max();
};

// Future arena construction appends every UP byte range and synthesized
// binding payload independently with this alignment. Planning applies the
// same policy with checked padding, so drawPayloadBytes is a safe upper bound.
inline constexpr std::size_t kV2ArenaDrawPayloadAppendAlignment =
    core::kSourcePayloadByteAlignment;

struct V2CpuReadyPlan {
  RawOrdinal rawOrdinal = 0;
  V2ReplayLane lane = V2ReplayLane::Legacy;
  V2ReplayReason reason = V2ReplayReason::InvalidImportedView;
  bool logicalSource = false;
  core::SourcePayloadCapacity capacity{};
  std::optional<core::SourcePayloadLayout> layout{};

  bool directArenaCandidate() const noexcept {
    return lane == V2ReplayLane::DirectArenaCandidate && logicalSource &&
           layout.has_value();
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
