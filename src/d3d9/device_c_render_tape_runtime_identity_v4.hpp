#pragma once

#include "device_c_render_tape_identity_v4.hpp"
#include "device_c_render_tape_replay_observer.hpp"
#include "../dxmt9/dxmt9_render_tape_runtime_observer.hpp"
#include "../dxmt9/dxmt9_render_tape_topology_observer.hpp"
#include "../dxmt9/dxmt9_render_tape_emission_observer.hpp"
#include "dxmt9/query_poll_snapshot.hpp"

#include <cstddef>
#include <memory>
#include <source_location>
#include <span>
#include <vector>

namespace dxmt9::core::metalqueue {
struct ReplaySourcePublicationReceipt;
}

namespace dxmt9::d3d9 {

struct RenderTapeRuntimeIdentityV4Limits {
  RenderTapeIdentityV3Limits provenance{};
  render::TapeTopologyLimits topology{4096u, 65536u, 4096u, 65536u, 65536u};
  std::uint32_t maxAttempts = 4096u;
};

// Replay-only local authority from one real setup END. Captured descriptor,
// cycle and ordinal labels stay owned by the executor, never by this root.
struct RenderTapeRuntimeQueryBaselineV4 {
  D9CWireObjectIdentity localIdentity{};
  core::QueryPollSnapshot snapshot{};
};

// An actual append/assembler supplies these original canonical locators;
// publication cannot infer them by matching record kinds or draw counts.
using RenderTapeRuntimeRecordLocator = render::TapeEmissionRecordLocator;

// Canonical origins bound by actual emission, not a semantic resource
// registry. A DrawRun contributes one row per subdraw; all other commands
// contribute one row with zero subdraw extent. kind uses the identity command
// alphabet, as in RenderTapeIdentityV4PhysicalCommand.
struct RenderTapeRuntimePublishedSourceOriginV4 {
  std::uint32_t physicalCommandIndex = 0u;
  std::uint32_t kind = 0u;
  std::uint32_t firstSubdraw = 0u;
  std::uint32_t subdrawCount = 0u;
  RenderTapeRuntimeRecordLocator record{};
  friend constexpr bool operator==(RenderTapeRuntimePublishedSourceOriginV4,
      RenderTapeRuntimePublishedSourceOriginV4) noexcept = default;
};

// Owned exact publication projection and total physical/subdraw origin map.
// No source payload, wrapper, resource bytes or borrowed spans are retained.
struct RenderTapeRuntimePublishedSourceOriginsV4 {
  render::TapeTopologyCaptureIdentity capture{};
  render::TapeTopologySourceIdentity source{};
  core::CpuReadySourceMetadata metadata{};
  std::size_t controlIndex = SIZE_MAX;
  bool hasPresent = false;
  std::vector<RenderTapeIdentityV4PhysicalCommand> commands{};
  std::vector<RenderTapeRuntimePublishedSourceOriginV4> origins{};
};

struct RenderTapeRuntimeIdentityV4TestAccess;
struct RenderTapeRuntimeIdentityFailure;
class RenderTapeControlledCpuV4Capability;
class RenderTapeExecutionV4;

// Retained cold root. Source publication copies neutral immutable extents,
// while actual transaction and Encode observations remain separate facts.
// No method changes a production transaction, selects a route, publishes a
// queue source, submits work or invents completion. All inputs borrow only
// during the call; in particular, no ReplayTransaction pointer is retained.
// Production replay/query mutations must run inside a short capture-lease
// visit. These value-only methods do not themselves retain endpoint admission;
// freeze/snapshot deliberately remain available after endpoint retirement.
class RenderTapeRuntimeIdentityV4 final : public render::TapeRuntimeCaptureAttachment {
public:
  RenderTapeRuntimeIdentityV4();
  ~RenderTapeRuntimeIdentityV4() override;
  bool begin(RenderTapeIdentityV4CaptureKey key, render::TapeRuntimeBaseline baseline,
      RenderTapeRuntimeIdentityV4Limits limits = {}) noexcept;
  // Called once inside the pre-publication capture-lease visit, even for an
  // empty batch. Successful admission enables strict replay-local continuity;
  // ordinary V4 captures and V3 do not enter this mode.
  bool seedQueryBaselines(RenderTapeIdentityV4CaptureKey key,
      std::span<const RenderTapeRuntimeQueryBaselineV4> baselines) noexcept;
  render::TapeRuntimeObserverSink sink() noexcept override;
  render::TapeTopologyCapture& topology() noexcept override { return topology_; }
  ReplayTransactionObserverSink replaySink() noexcept;
  bool beginRecord(std::uint32_t recordIndex, std::uint32_t recordType) noexcept;
  bool beginDispatch() noexcept;
  bool endRecord(std::int32_t result) noexcept;
  bool bindEmission(query::WriterIdentity writer, std::uint32_t physicalCommandIndex,
      core::MetalCommandKind kind, std::span<const RenderTapeRuntimeRecordLocator> records,
      std::uint32_t firstSubdraw = 0u) noexcept;
  bool acknowledgeRaw(ReplayTransactionRawIdentity raw, bool ledgerSucceeded) noexcept;
  bool setCut(std::uint64_t eventOrdinal, RenderTapeIdentityV3CutKind kind,
      std::uint32_t record = 0u) noexcept;
  void queryIssue(D9CWireObjectIdentity identity, std::uint32_t flags,
      const core::QueryPollSnapshot& snapshot, std::int32_t result,
      std::uint64_t issueCycle = 0u) noexcept;
  void queryPoll(D9CWireObjectIdentity identity,
      const core::QueryPollSnapshot& snapshot, std::int32_t result) noexcept;
  bool snapshot(RenderTapeIdentityV4CaptureKey key, RenderTapeIdentityV4View& identity,
      RenderTapeExecutionTopologyV1View& topology, bool requireComplete = true) noexcept;
  // Cold value-only pull, outside the queue mutex. An exact published source
  // is available before coordinator endRecord/raw ACK, independently of other
  // open records. Frozen retained roots remain readable. Invalid/stale queries
  // and allocation failures clear out without poisoning the capture.
  bool copyPublishedSourceOrigins(render::TapeTopologyCaptureIdentity capture,
      const core::metalqueue::ReplaySourcePublicationReceipt& publication,
      RenderTapeRuntimePublishedSourceOriginsV4& out) noexcept;
  // Resolve exactly one already-owned publication by its full actual source
  // locator and sequence, never by fabricated metadata/control placement.
  // This numeric locator grants no source-storage or lifetime authority.
  bool copyPublishedSourceOrigins(render::TapeTopologyCaptureIdentity capture,
      core::CpuReadyTape::SourceRef source, std::uint64_t sequence,
      RenderTapeRuntimePublishedSourceOriginsV4& out) noexcept;
  void fail(std::source_location location = std::source_location::current()) noexcept;
  bool failed() const noexcept;
  RenderTapeRuntimeIdentityFailure firstFailure() const noexcept;
  RenderTapeRuntimeIdentityFailure diagnosticState() const noexcept;
  bool matches(RenderTapeIdentityV4CaptureKey key) const noexcept;
  bool freeze(RenderTapeIdentityV4CaptureKey key, std::uint64_t presentOrdinal) noexcept;
  bool frozen() const noexcept;

private:
  friend class RenderTapeControlledCpuV4Capability;
  friend class RenderTapeExecutionV4;
  friend struct RenderTapeRuntimeIdentityV4TestAccess;
  bool snapshotControlledCpu(RenderTapeIdentityV4CaptureKey,
      RenderTapeIdentityV4View&) noexcept;
  bool copyPublishedSources(RenderTapeIdentityV4CaptureKey, std::size_t& cursor,
      std::vector<core::metalqueue::ReplaySourcePublicationReceipt>&) noexcept;
  bool ownsWritingContinuation(RenderTapeIdentityV4CaptureKey, query::WriterIdentity,
      core::CpuReadyTape::SourceRef, std::size_t commandCount) noexcept;
  class State;
  static bool observeReplay(void*, const ReplayTransactionObservation&) noexcept;
  static void replayFailed(void*, ReplayTransactionObserverFailure) noexcept;
  std::unique_ptr<State> state_{};
  render::TapeTopologyCapture topology_{};
};

} // namespace dxmt9::d3d9
