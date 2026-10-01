#pragma once

#include "device_c_render_tape.hpp"
#include "dxmt9/query_poll_snapshot.hpp"

namespace dxmt9::d3d9 {

// Capture-only bookkeeping is enabled once, before the first public Issue.
// It lives beside the existing retained receipt, not in the core Query or a
// hot-path signature. Cycle and Issue ordinal are deliberately independent.
bool renderTapeEventQueryCheckpointTrackingEnabled() noexcept;

inline bool renderTapeObserveEventQueryCheckpointIssue(
    RenderTapeQueryCycleState& cycle, std::uint64_t& epoch,
    std::uint32_t flags, const core::QueryPollSnapshot& snapshot) noexcept {
  if (snapshot.type != core::QueryType::Event || snapshot.failed ||
      !snapshot.identity || !snapshot.issueDeviceEpoch) return false;
  auto next = cycle;
  next.type = 8u;
  if (!renderTapeAdvanceQueryCycle(next, flags, false) ||
      next.issueOrdinal != snapshot.issueOrdinal ||
      next.active != snapshot.active) return false;
  cycle = next;
  epoch = snapshot.issueDeviceEpoch;
  return true;
}

struct RenderTapeEventQueryCheckpointAuthority {
  std::uint64_t coreQueryIdentity = 0u;
  std::uint64_t issueCycle = 0u;
  std::uint64_t issueOrdinal = 0u;
  std::uint64_t deviceEpoch = 0u;
  std::uint64_t rawOrdinal = 0u;
  std::uint64_t queue = 0u;
  std::uint64_t sequence = 0u;
  std::uint64_t initializer = 0u;
  bool active = false;
  bool failed = false;
  bool cancelled = false;
  bool mappingValid = false;

  friend constexpr bool operator==(
      const RenderTapeEventQueryCheckpointAuthority&,
      const RenderTapeEventQueryCheckpointAuthority&) = default;
};

inline RenderTapeEventQueryCheckpointAuthority
renderTapeEventQueryCheckpointAuthority(
    const RenderTapeQueryCycleState& cycle, std::uint64_t epoch,
    bool mappingValid, std::uint64_t rawOrdinal,
    const core::QueryPollSnapshot& snapshot) noexcept {
  const auto fence = snapshot.backendFence.snapshot();
  return {
      snapshot.identity, cycle.issueCycle, snapshot.issueOrdinal,
      snapshot.issueDeviceEpoch, rawOrdinal, fence.queue, fence.sequence,
      fence.initializer, snapshot.active, snapshot.failed,
      snapshot.backendFence.marker && snapshot.backendFence.marker->cancelled(),
      mappingValid && cycle.type == 8u &&
          snapshot.type == core::QueryType::Event &&
          cycle.issueOrdinal == snapshot.issueOrdinal &&
          cycle.active == snapshot.active && epoch == snapshot.issueDeviceEpoch,
  };
}

// Ready is a CPU replay proof only. Successful non-FLUSH backend polling is
// a separate required proof, followed by the same-current-issue recheck.
constexpr bool renderTapeEventQueryCheckpointAuthorityAdmitted(
    std::uint64_t expectedCycle, std::uint64_t expectedIssue,
    const RenderTapeEventQueryCheckpointAuthority& authority,
    std::uint64_t capturedRawOrdinal, bool replayReady) noexcept {
  return replayReady && authority.mappingValid &&
      authority.coreQueryIdentity && authority.deviceEpoch && authority.queue &&
      expectedCycle && expectedIssue &&
      authority.issueCycle == expectedCycle &&
      authority.issueOrdinal == expectedIssue &&
      authority.rawOrdinal == capturedRawOrdinal &&
      !authority.active && !authority.failed && !authority.cancelled;
}

} // namespace dxmt9::d3d9
