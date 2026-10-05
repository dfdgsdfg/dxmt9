#pragma once

#include "device_c_render_tape_identity_v4.hpp"

namespace dxmt9::d3d9 {

inline constexpr std::uint64_t kRenderTapeExecutionTopologyV1Magic = UINT64_C(0x31504f5454523944);
inline constexpr std::uint32_t kRenderTapeExecutionTopologyV1Version = 1u;
inline constexpr char kRenderTapeExecutionTopologyV1Schema[] =
    "dxmt9.render_tape.execution_topology.v1";
inline constexpr std::uint32_t kRenderTapeExecutionTopologyV1NoIndex = UINT32_MAX;

enum class RenderTapeExecutionTopologyV1Route : std::uint32_t {
  NaturalOrder = 1u, PlannerBypass = 2u, Optimized = 3u,
};
enum class RenderTapeExecutionTopologyV1Outcome : std::uint32_t {
  ExecutedDagPass = 1u, ExecutedWithoutDag = 2u, ProvenOmitted = 3u,
};
enum class RenderTapeExecutionTopologyV1OmissionReason : std::uint32_t {
  None = 0u, DeadPass = 1u,
};
enum class RenderTapeExecutionTopologyV1FallbackReason : std::uint32_t {
  None = 0u, StrictProfile, FeaturesOff, QueryScope, ExplicitSkip,
  InvalidPlan, FrontierRollback, TraditionalBackend, ExternalCommittedPlan,
  DceEncodedPrefix,
};
constexpr bool renderTapeExecutionTopologyV1FallbackReasonAdmitted(std::uint32_t reason) noexcept {
  return reason <= static_cast<std::uint32_t>(
      RenderTapeExecutionTopologyV1FallbackReason::DceEncodedPrefix);
}
constexpr bool renderTapeExecutionTopologyV1RouteAdmitted(std::uint32_t route) noexcept {
  return route >= static_cast<std::uint32_t>(RenderTapeExecutionTopologyV1Route::NaturalOrder) &&
      route <= static_cast<std::uint32_t>(RenderTapeExecutionTopologyV1Route::Optimized);
}

struct RenderTapeExecutionTopologyV1Header {
  std::uint64_t magic = kRenderTapeExecutionTopologyV1Magic;
  std::uint64_t identityBytes = 0u, eventsBytes = 0u;
  std::uint64_t captureToken = 0u, captureEpoch = 0u, queue = 0u;
  RenderTapeDigest identityDigest{}, eventsDigest{};
  std::uint32_t version = kRenderTapeExecutionTopologyV1Version;
  std::uint32_t headerSize = sizeof(RenderTapeExecutionTopologyV1Header);
  RenderTapeIdentityV3Table plans{}, placements{};
  std::uint32_t reserved0 = 0u, reserved1 = 0u;
};

// One accepted production invocation. Prefix and suffix invocations have
// distinct plan IDs, even when one source participates in both.
struct RenderTapeExecutionTopologyV1Plan {
  std::uint64_t planId = 0u, acceptanceEdge = 0u;
  std::uint32_t route = 0u, fallbackReason = 0u;
  std::uint32_t firstPlacement = 0u, placementCount = 0u, executedCount = 0u;
  std::uint32_t reserved0 = 0u;
};

// Group equality is exactly (accepted plan ID, dagPassIndex), never a
// source-local pass number and never physical Metal encoder equivalence.
struct RenderTapeExecutionTopologyV1Placement {
  std::uint32_t planIndex = kRenderTapeExecutionTopologyV1NoIndex;
  std::uint32_t commandIndex = kRenderTapeExecutionTopologyV1NoIndex;
  std::uint32_t outcome = 0u;
  std::uint32_t executionOrdinal = kRenderTapeExecutionTopologyV1NoIndex;
  std::uint32_t dagPassIndex = kRenderTapeExecutionTopologyV1NoIndex;
  std::uint32_t passKind = 0u, omissionReason = 0u, reserved0 = 0u;
};
struct RenderTapeExecutionTopologyV1View {
  RenderTapeExecutionTopologyV1Header header{};
  std::vector<RenderTapeExecutionTopologyV1Plan> plans{};
  std::vector<RenderTapeExecutionTopologyV1Placement> placements{};
};
constexpr bool renderTapeExecutionTopologyV1PlacementAdmitted(
    const RenderTapeExecutionTopologyV1Plan& plan,
    const RenderTapeExecutionTopologyV1Placement& placement) noexcept {
  if (!renderTapeExecutionTopologyV1RouteAdmitted(plan.route) || placement.reserved0)
    return false;
  const bool optimized = plan.route ==
      static_cast<std::uint32_t>(RenderTapeExecutionTopologyV1Route::Optimized);
  switch (static_cast<RenderTapeExecutionTopologyV1Outcome>(placement.outcome)) {
  case RenderTapeExecutionTopologyV1Outcome::ExecutedDagPass:
    return optimized && placement.executionOrdinal != kRenderTapeExecutionTopologyV1NoIndex &&
        placement.dagPassIndex != kRenderTapeExecutionTopologyV1NoIndex &&
        placement.passKind >= 1u && placement.passKind <= 5u && !placement.omissionReason;
  case RenderTapeExecutionTopologyV1Outcome::ExecutedWithoutDag:
    return placement.executionOrdinal != kRenderTapeExecutionTopologyV1NoIndex &&
        placement.dagPassIndex == kRenderTapeExecutionTopologyV1NoIndex &&
        !placement.passKind && !placement.omissionReason;
  case RenderTapeExecutionTopologyV1Outcome::ProvenOmitted:
    return optimized && placement.executionOrdinal == kRenderTapeExecutionTopologyV1NoIndex &&
        placement.dagPassIndex == kRenderTapeExecutionTopologyV1NoIndex && !placement.passKind &&
        placement.omissionReason == static_cast<std::uint32_t>(
            RenderTapeExecutionTopologyV1OmissionReason::DeadPass);
  }
  return false;
}
constexpr bool renderTapeExecutionTopologyV1CommandPlacementAdmitted(
    const RenderTapeIdentityV4PhysicalCommand& command,
    const RenderTapeExecutionTopologyV1Plan& plan,
    const RenderTapeExecutionTopologyV1Placement& placement) noexcept {
  if (!renderTapeExecutionTopologyV1PlacementAdmitted(plan, placement)) return false;
  const auto outcome = static_cast<RenderTapeExecutionTopologyV1Outcome>(placement.outcome);
  if (outcome == RenderTapeExecutionTopologyV1Outcome::ProvenOmitted)
    return command.coordinatorIndex == kRenderTapeIdentityV4NoIndex &&
        command.kind != static_cast<std::uint32_t>(RenderTapeIdentityV3CommandKind::Present) &&
        command.kind != static_cast<std::uint32_t>(RenderTapeIdentityV3CommandKind::Readback);
  return outcome != RenderTapeExecutionTopologyV1Outcome::ExecutedDagPass ||
      !command.drawCount || placement.passKind == 1u;
}
enum class RenderTapeExecutionTopologyV1Status : std::uint8_t {
  Valid, InvalidIdentity, InvalidHeader, InvalidLayout, BindingMismatch,
  InvalidPlan, InvalidPlacement, Incomplete, CapacityExceeded, AllocationFailed,
  UnsupportedLifecycle,
};
static_assert(sizeof(RenderTapeExecutionTopologyV1Header) == 152u);
static_assert(sizeof(RenderTapeExecutionTopologyV1Plan) == 40u);
static_assert(sizeof(RenderTapeExecutionTopologyV1Placement) == 32u);
static_assert(std::is_trivially_copyable_v<RenderTapeExecutionTopologyV1Header>);
static_assert(std::is_trivially_copyable_v<RenderTapeExecutionTopologyV1Plan>);
static_assert(std::is_trivially_copyable_v<RenderTapeExecutionTopologyV1Placement>);

RenderTapeExecutionTopologyV1Status validateRenderTapeExecutionTopologyV1Rows(
    const RenderTapeIdentityV4View& identity,
    const RenderTapeExecutionTopologyV1View& topology) noexcept;
// Rows validates staged values against an already admitted identity view;
// only the wire pair validator authenticates byte/digest binding. Execution
// ordinals are dense within each accepted plan, not physical table indices.
RenderTapeExecutionTopologyV1Status validateRenderTapeExecutionTopologyV1(
    std::span<const std::byte> tape, const RenderTapeBlobCatalogue& catalogue,
    std::span<const std::byte> identity, std::span<const std::byte> topology,
    RenderTapeExecutionTopologyV1View* out = nullptr) noexcept;
std::vector<std::byte> buildRenderTapeExecutionTopologyV1(
    std::span<const std::byte> tape, const RenderTapeBlobCatalogue& catalogue,
    std::span<const std::byte> identity,
    const RenderTapeExecutionTopologyV1View& topology,
    RenderTapeExecutionTopologyV1Status* status = nullptr);

} // namespace dxmt9::d3d9
