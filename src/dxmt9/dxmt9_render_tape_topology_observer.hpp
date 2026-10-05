#pragma once

#include "dxmt9_cpu_ready_tape.hpp"
#include "framegraph/fg_dag.hpp"
#include "../d3d9/device_c_render_tape_execution_topology_v1.hpp"

#include <mutex>
#include <span>
#include <vector>

namespace dxmt9::render {

struct TapeTopologyCaptureIdentity {
  std::uint64_t token = 0u;
  std::uint64_t epoch = 0u;
  std::uint64_t queue = 0u;

  constexpr bool valid() const noexcept { return token && epoch && queue; }
  friend constexpr bool operator==(TapeTopologyCaptureIdentity,
                                   TapeTopologyCaptureIdentity) noexcept = default;
};

// Actual publication identity, copied before the payload can be reclaimed.
// This is not a submission, completion, pass or physical-encoder receipt.
struct TapeTopologySourceIdentity {
  core::CpuReadyTape::SourceRef source{};
  std::uint64_t sourceOrdinal = 0u;
  std::uint64_t sequence = 0u;
  std::uint64_t publicationEdge = 0u;
  std::uint32_t sourceIndex = UINT32_MAX;
  std::uint32_t commandCount = 0u;
  friend constexpr bool operator==(TapeTopologySourceIdentity,
                                   TapeTopologySourceIdentity) noexcept = default;
};

struct TapeTopologyPhysicalExtent {
  std::uint32_t physicalCommandIndex = 0u;
  std::uint32_t kind = 0u;
  std::uint32_t drawCount = 0u;
  std::uint32_t coordinatorIndex = d3d9::kRenderTapeIdentityV4NoIndex;
  friend constexpr bool operator==(TapeTopologyPhysicalExtent,
                                   TapeTopologyPhysicalExtent) noexcept = default;
};

// A value-qualified reference into the neutral V4 physical-command table.
// Every source/storage generation and extent is checked again at acceptance.
struct TapeTopologyCommandRef {
  TapeTopologySourceIdentity owner{};
  std::uint32_t commandIndex = UINT32_MAX;
  TapeTopologyPhysicalExtent extent{};
  friend constexpr bool operator==(TapeTopologyCommandRef,
                                   TapeTopologyCommandRef) noexcept = default;
};

struct TapeTopologyAcceptanceIdentity {
  TapeTopologyCaptureIdentity capture{};
  std::uint64_t attempt = 0u;
  std::uint64_t edge = 0u;
};

struct TapeTopologyEncodedInvocationIdentity {
  TapeTopologyCaptureIdentity capture{};
  std::uint64_t attempt = 0u;
};

struct TapeTopologyAcceptedInvocation {
  std::uint64_t attempt = 0u;
  std::uint64_t edge = 0u;
};

struct TapeTopologyLimits {
  std::uint32_t maxSources = 1024u;
  std::uint32_t maxCommands = 4096u;
  std::uint32_t maxPlans = 1024u;
  std::uint32_t maxGraphPasses = 8192u;
  std::uint32_t maxGraphCommandRefs = 32768u;
};

// Runtime facts, not a wire sidecar: there is deliberately no events/identity
// digest authority. The final owner seals and authenticates the companion.
struct TapeTopologyRuntimeSnapshot {
  TapeTopologyCaptureIdentity capture{};
  std::vector<d3d9::RenderTapeExecutionTopologyV1Plan> plans{};
  std::vector<d3d9::RenderTapeExecutionTopologyV1Placement> placements{};
  bool complete = false;
};

constexpr std::uint32_t tapeTopologyCommandKind(core::MetalCommandKind kind) noexcept {
  using Command = d3d9::RenderTapeIdentityV3CommandKind;
  switch (kind) {
  case core::MetalCommandKind::DrawRun: return static_cast<std::uint32_t>(Command::DrawRun);
  case core::MetalCommandKind::Clear: return static_cast<std::uint32_t>(Command::Clear);
  case core::MetalCommandKind::SurfaceCopy: return static_cast<std::uint32_t>(Command::SurfaceCopy);
  case core::MetalCommandKind::StretchRect: return static_cast<std::uint32_t>(Command::StretchRect);
  case core::MetalCommandKind::Readback: return static_cast<std::uint32_t>(Command::Readback);
  case core::MetalCommandKind::ColorFill: return static_cast<std::uint32_t>(Command::ColorFill);
  case core::MetalCommandKind::DepthResolve: return static_cast<std::uint32_t>(Command::DepthResolve);
  case core::MetalCommandKind::GenerateMipmaps: return static_cast<std::uint32_t>(Command::GenerateMipmaps);
  case core::MetalCommandKind::Present: return static_cast<std::uint32_t>(Command::Present);
  }
  return static_cast<std::uint32_t>(Command::Invalid);
}

struct TapeTopologyCaptureTestAccess;

// Cold capture-owned ledger. It neither chooses a route nor builds a graph.
// Stage inputs borrow only during the call; accepted facts are owned values.
// Capture failure never authorizes queue work, a fallback or a GPU terminal.
class TapeTopologyCapture {
public:
  bool begin(TapeTopologyCaptureIdentity capture,
      std::uint64_t baselineEdge, TapeTopologyLimits limits = {}) noexcept;
  bool publish(TapeTopologySourceIdentity source,
      std::span<const TapeTopologyPhysicalExtent> extents) noexcept;
  bool commandRef(std::uint32_t index, TapeTopologyCommandRef& out) const noexcept;
  // Exact published-source lookup for backend-local command cursors. Results
  // are copied values, never payload pointers or authority to register/encode.
  // Historical published generations stay distinct for delayed acceptance;
  // mixing their generation/storage identity with another sequence rejects.
  bool commandRef(core::CpuReadyTape::SourceRef source, std::uint64_t sequence,
      std::uint32_t physicalCommandIndex, TapeTopologyCommandRef& out) const noexcept;
  bool sourceIdentity(core::CpuReadyTape::SourceRef source, std::uint64_t sequence,
      TapeTopologySourceIdentity& out) const noexcept;
  // Copy only under an admitted retained-lease visit at production call sites.
  TapeTopologyCaptureIdentity identity() const noexcept;

  // commands is in immutable physical-table order. executionOrder indexes
  // that call-local list. NaturalOrder must preserve it; PlannerBypass may
  // retain an externally selected order, but grants no DAG membership.
  std::uint64_t stageWithoutDag(d3d9::RenderTapeExecutionTopologyV1Route route,
      d3d9::RenderTapeExecutionTopologyV1FallbackReason reason,
      std::span<const TapeTopologyCommandRef> commands,
      std::span<const std::uint32_t> executionOrder) noexcept;

  // graphCommands maps the actual selected graph's flattened command indices
  // to immutable refs. alreadyEncoded is the exact live-plan prefix observed
  // encoded by earlier pending/accepted invocations; it is excluded, never
  // regrouped retrospectively. Pending prefix facts are not registration.
  // actualOrder must equal the selected live graph order after that prefix.
  std::uint64_t stageSelectedDag(
      d3d9::RenderTapeExecutionTopologyV1FallbackReason reason,
      const framegraph::FrameGraph& graph,
      std::span<const TapeTopologyCommandRef> graphCommands,
      std::span<const std::uint32_t> actualOrder,
      std::span<const std::uint32_t> alreadyEncoded = {}) noexcept;

  // The actual successful Encode return observes an effect, not accepted
  // topology, source registration, submission or GPU completion. execution
  // order is checked against the owned selected invocation before recording.
  bool observeEncoded(TapeTopologyEncodedInvocationIdentity invocation,
      std::span<const TapeTopologyCommandRef> executionOrder) noexcept;

  // Only the integration's actual accepted ownership/registration receipt
  // supplies these sources. Exact equality alone is not queue authority.
  bool accept(TapeTopologyAcceptanceIdentity acceptance,
      std::span<const TapeTopologySourceIdentity> registeredSources) noexcept;
  // Records a chronological unaccepted-invocation prefix atomically. Actual
  // registration may precede or follow Encode. Only invocations with both
  // facts publish rows, in invocation order, after all validation succeeds.
  bool acceptBatch(TapeTopologyCaptureIdentity capture,
      std::span<const TapeTopologyAcceptedInvocation> invocations,
      std::span<const TapeTopologySourceIdentity> registeredSources) noexcept;
  bool reject(std::uint64_t attempt) noexcept;
  // Complete requires registration and successful invocation execution for
  // every command. Partial inspection is explicitly non-authoritative.
  // Submission, inline terminal and GPU completion remain separate domains.
  bool snapshot(TapeTopologyRuntimeSnapshot& out,
      bool requireComplete = true) const noexcept;
  void fail() noexcept;
  void revoke() noexcept;
  bool failed() const noexcept;

private:
  friend struct TapeTopologyCaptureTestAccess;
  struct Source {
    TapeTopologySourceIdentity identity{};
    std::uint32_t firstCommand = 0u;
  };
  struct Command {
    TapeTopologyCommandRef ref{};
    std::uint32_t acceptedPlacement = UINT32_MAX;
    std::uint64_t pendingAttempt = 0u;
  };
  struct StageCommand {
    TapeTopologyCommandRef ref{};
    d3d9::RenderTapeExecutionTopologyV1Placement placement{};
    bool accounted = false;
    bool prior = false;
  };
  struct PendingCommand {
    TapeTopologyCommandRef ref{};
    d3d9::RenderTapeExecutionTopologyV1Placement placement{};
  };
  struct PendingInvocation {
    d3d9::RenderTapeExecutionTopologyV1Plan plan{};
    std::uint32_t firstCommand = 0u;
    std::uint32_t publishedPlanIndex = UINT32_MAX;
    bool accepted = false;
    bool encoded = false;
  };
  bool invalidate() noexcept { failed_ = true; return false; }
  bool prepare(d3d9::RenderTapeExecutionTopologyV1Route route,
      d3d9::RenderTapeExecutionTopologyV1FallbackReason reason,
      std::span<const TapeTopologyCommandRef> refs,
      bool permitPrior);
  std::uint64_t sealStage();
  bool matches(const TapeTopologyCommandRef& ref) const noexcept;
  bool priorExecution(const Command& command, std::uint64_t& attempt,
      std::uint32_t& ordinal) const noexcept;
  void promoteTerminalPrefix() noexcept;

  mutable std::mutex mutex_{};
  TapeTopologyCaptureIdentity capture_{};
  TapeTopologyLimits limits_{};
  std::uint64_t baselineEdge_ = 0u;
  std::uint64_t lastAcceptanceEdge_ = 0u;
  std::uint64_t nextAttempt_ = 1u;
  std::vector<Source> sources_{};
  std::vector<Command> commands_{};
  std::vector<d3d9::RenderTapeExecutionTopologyV1Plan> plans_{};
  std::vector<d3d9::RenderTapeExecutionTopologyV1Placement> placements_{};
  std::vector<PendingInvocation> pending_{};
  std::vector<PendingCommand> pendingCommands_{};
  std::vector<StageCommand> staged_{};
  std::vector<std::uint32_t> liveOrder_{};
  d3d9::RenderTapeExecutionTopologyV1Plan stagedPlan_{};
  bool active_ = false;
  bool failed_ = false;
};

inline thread_local TapeTopologyCapture* activeTapeTopologyCapture = nullptr;

// Install only inside the capture-enabled branch around synchronous backend
// work. No diagnostic field enters an execution payload or hot signature.
class ScopedTapeTopologyCapture {
public:
  explicit ScopedTapeTopologyCapture(TapeTopologyCapture& owner) noexcept
      : previous_(activeTapeTopologyCapture) { activeTapeTopologyCapture = &owner; }
  ~ScopedTapeTopologyCapture() { activeTapeTopologyCapture = previous_; }
  ScopedTapeTopologyCapture(const ScopedTapeTopologyCapture&) = delete;
  ScopedTapeTopologyCapture& operator=(const ScopedTapeTopologyCapture&) = delete;
private:
  TapeTopologyCapture* previous_ = nullptr;
};

} // namespace dxmt9::render
