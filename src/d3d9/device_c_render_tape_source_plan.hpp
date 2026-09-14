#pragma once

#include "device_c_chunk_replay.hpp"
#include "device_c_render_tape_capture.hpp"
#include "device_c_render_tape_identity.hpp"
#include "device_c_render_tape_runtime_objects.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace dxmt9::d3d9 {

enum class CaptureSourceReplayPlanStatus : std::uint8_t {
  Valid,
  InvalidInput,
  InvalidIdentity,
  WrongAuthority,
  MissingSettlements,
  EventMappingMismatch,
  SourceCoverageMismatch,
  AllocationFailed,
};

// One immutable capture-owned source interval.  `capturedSeqId` is copied
// exactly from the authenticated sidecar; the planner never allocates or
// rebases queue identity.
struct CaptureSourceReplayRow {
  std::uint64_t eventOrdinal = 0u;
  std::uint64_t sourceOrdinal = 0u;
  std::uint64_t capturedSeqId = 0u;
  std::uint32_t commandChunkIndex = 0u;
  std::uint32_t firstRecord = 0u;
  std::uint32_t recordCount = 0u;
};

// A cold, value-owned replay admission result.  All accessors are read-only;
// the source rows and per-chunk cuts are published atomically by the builder.
// This is a QueuePublication cut contract: it preserves captured source
// provenance, but does not claim physical Metal pass or encoder equivalence.
class CaptureSourceReplayPlan {
public:
  CaptureSourceReplayPlan() = default;
  CaptureSourceReplayPlan(const CaptureSourceReplayPlan&) = delete;
  CaptureSourceReplayPlan& operator=(const CaptureSourceReplayPlan&) = delete;
  CaptureSourceReplayPlan(CaptureSourceReplayPlan&& other) noexcept;
  CaptureSourceReplayPlan& operator=(CaptureSourceReplayPlan&& other) noexcept;

  bool valid() const noexcept {
    return status_ == CaptureSourceReplayPlanStatus::Valid;
  }
  CaptureSourceReplayPlanStatus status() const noexcept { return status_; }

  std::span<const CaptureSourceReplayRow> rows() const noexcept {
    return rows_;
  }
  std::span<const ProviderReplayRecordRange> rangesForCommandChunk(
      std::size_t commandChunkIndex) const noexcept;
  std::size_t commandChunkCount() const noexcept {
    return rangesByCommandChunk_.size();
  }

  const RenderTapeDigest& digest() const noexcept { return digest_; }

private:
  friend CaptureSourceReplayPlanStatus buildCaptureSourceReplayPlan(
      std::span<const std::byte> tape,
      const render_tape_runtime::GeneralPreflightPlan& plan,
      std::span<const std::byte> identitySidecar,
      CaptureSourceReplayPlan& out) noexcept;

  CaptureSourceReplayPlanStatus status_ =
      CaptureSourceReplayPlanStatus::InvalidInput;
  std::vector<CaptureSourceReplayRow> rows_{};
  std::vector<std::vector<ProviderReplayRecordRange>>
      rangesByCommandChunk_{};
  RenderTapeDigest digest_{};
};

// Transactional output: failure leaves `out` unchanged, including a prior valid
// plan. Callers must check the returned status before using it for a new input.
[[nodiscard]] CaptureSourceReplayPlanStatus buildCaptureSourceReplayPlan(
    std::span<const std::byte> tape,
    const render_tape_runtime::GeneralPreflightPlan& plan,
    std::span<const std::byte> identitySidecar,
    CaptureSourceReplayPlan& out) noexcept;

const char* captureSourceReplayPlanStatusName(
    CaptureSourceReplayPlanStatus status) noexcept;

}  // namespace dxmt9::d3d9
