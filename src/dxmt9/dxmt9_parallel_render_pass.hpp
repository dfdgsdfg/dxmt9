#pragma once

#include "dxmt9_encode_partition.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>

namespace dxmt9::encoders {

inline constexpr std::size_t kParallelRenderPassChildCapacity = 16u;

// A value proof that a child can establish its first draw without borrowing
// the coordinator's mutable native binding shadow. The producer owns the
// meaning of generation; zero is never a complete snapshot.
struct ParallelFirstDrawSnapshot {
  std::uint64_t generation = 0;
  bool complete = false;

  friend constexpr bool operator==(const ParallelFirstDrawSnapshot&,
                                   const ParallelFirstDrawSnapshot&) = default;
};

enum class ParallelPassFallbackReason : std::uint8_t {
  None,
  NotRequested,
  NoExplicitPlan,
  PlanNotValidated,
  PassNotSealed,
  TooFewChildren,
  ChildCapacity,
  CoordinatorCommand,
  Query,
  Clear,
  SidecarObservation,
  InitializerWait,
  Present,
  UnresolvedHazard,
  FirstDrawSnapshotMissing,
  ParallelEncoderUnavailable,
  InvalidCompletionOrder,
  ParentPreparationFailed,
  ChildCreationFailed,
};

struct ParallelPassEligibilityInput {
  std::span<const EncodePartitionRangeSnapshot> ranges{};
  std::span<const ParallelFirstDrawSnapshot> firstDrawSnapshots{};
  bool explicitPlan = false;
  bool planValidated = false;
  bool logicalPassSealed = false;
  bool hasQuery = false;
  bool hasClear = false;
  bool hasSidecarObservation = false;
  bool hasInitializerWait = false;
  bool hasPresent = false;
  bool hasUnresolvedHazard = false;
};

struct ParallelPassEligibilityDecision {
  ParallelPassFallbackReason fallback =
      ParallelPassFallbackReason::NoExplicitPlan;
  std::uint32_t childCount = 0;
  bool considered = false;
  bool eligible = false;

  friend constexpr bool operator==(
      const ParallelPassEligibilityDecision&,
      const ParallelPassEligibilityDecision&) = default;
};

// This surface is deliberately Metal-free. It accepts only a complete,
// validated production plan and rejects every coordinator command or semantic
// fence before any child can be created.
inline ParallelPassEligibilityDecision classifyParallelPassEligibility(
    const ParallelPassEligibilityInput& input) noexcept {
  ParallelPassEligibilityDecision result{.considered = true};
  auto reject = [&](ParallelPassFallbackReason reason) {
    result.fallback = reason;
    return result;
  };
  if (!input.explicitPlan) {
    return reject(ParallelPassFallbackReason::NoExplicitPlan);
  }
  if (!input.planValidated) {
    return reject(ParallelPassFallbackReason::PlanNotValidated);
  }
  if (!input.logicalPassSealed) {
    return reject(ParallelPassFallbackReason::PassNotSealed);
  }
  if (input.ranges.size() < 2u) {
    return reject(ParallelPassFallbackReason::TooFewChildren);
  }
  if (input.ranges.size() > kParallelRenderPassChildCapacity) {
    return reject(ParallelPassFallbackReason::ChildCapacity);
  }
  if (input.hasQuery) {
    return reject(ParallelPassFallbackReason::Query);
  }
  if (input.hasClear) {
    return reject(ParallelPassFallbackReason::Clear);
  }
  if (input.hasSidecarObservation) {
    return reject(ParallelPassFallbackReason::SidecarObservation);
  }
  if (input.hasInitializerWait) {
    return reject(ParallelPassFallbackReason::InitializerWait);
  }
  if (input.hasPresent) {
    return reject(ParallelPassFallbackReason::Present);
  }
  if (input.hasUnresolvedHazard) {
    return reject(ParallelPassFallbackReason::UnresolvedHazard);
  }
  if (input.firstDrawSnapshots.size() != input.ranges.size()) {
    return reject(ParallelPassFallbackReason::FirstDrawSnapshotMissing);
  }
  for (std::size_t i = 0; i < input.ranges.size(); ++i) {
    if (input.ranges[i].kind != EncodePartitionRangeKind::DrawRunEntries) {
      return reject(ParallelPassFallbackReason::CoordinatorCommand);
    }
    if (!input.firstDrawSnapshots[i].complete ||
        input.firstDrawSnapshots[i].generation == 0u) {
      return reject(ParallelPassFallbackReason::FirstDrawSnapshotMissing);
    }
  }
  result.fallback = ParallelPassFallbackReason::None;
  result.childCount = static_cast<std::uint32_t>(input.ranges.size());
  result.eligible = true;
  return result;
}

struct ParallelPassExecutionDecision {
  ParallelPassFallbackReason fallback =
      ParallelPassFallbackReason::NotRequested;
  bool considered = false;
  bool eligible = false;
  bool selected = false;

  friend constexpr bool operator==(const ParallelPassExecutionDecision&,
                                   const ParallelPassExecutionDecision&) =
      default;
};

inline ParallelPassExecutionDecision decideParallelPassExecution(
    bool requested,
    const ParallelPassEligibilityDecision& eligibility,
    bool parallelEncoderAvailable) noexcept {
  if (!requested) {
    return {};
  }
  ParallelPassExecutionDecision result{
      .fallback = eligibility.fallback,
      .considered = eligibility.considered,
      .eligible = eligibility.eligible,
  };
  if (!eligibility.eligible) {
    return result;
  }
  if (!parallelEncoderAvailable) {
    result.fallback = ParallelPassFallbackReason::ParallelEncoderUnavailable;
    return result;
  }
  result.fallback = ParallelPassFallbackReason::None;
  result.selected = true;
  return result;
}

struct ParallelPassChildPlan {
  EncodePartitionRangeSnapshot range{};
  ParallelFirstDrawSnapshot firstDraw{};
  std::uint32_t childOrdinal = 0;
  std::uint32_t localShadowOrdinal = 0;
  bool forceFullFirstDrawBinding = true;
};

struct ParallelPassPlanStorage {
  std::array<ParallelPassChildPlan, kParallelRenderPassChildCapacity> children{};
  std::size_t count = 0;

  void reset() noexcept { count = 0; }
  std::span<const ParallelPassChildPlan> view() const noexcept {
    return std::span<const ParallelPassChildPlan>(children.data(), count);
  }
};

inline ParallelPassEligibilityDecision planParallelRenderPassChildren(
    const ParallelPassEligibilityInput& input,
    ParallelPassPlanStorage& storage) noexcept {
  storage.reset();
  const auto decision = classifyParallelPassEligibility(input);
  if (!decision.eligible) {
    return decision;
  }
  for (std::size_t i = 0; i < input.ranges.size(); ++i) {
    storage.children[i] = ParallelPassChildPlan{
        .range = input.ranges[i],
        .firstDraw = input.firstDrawSnapshots[i],
        .childOrdinal = static_cast<std::uint32_t>(i),
        .localShadowOrdinal = static_cast<std::uint32_t>(i + 1u),
    };
  }
  storage.count = input.ranges.size();
  return decision;
}

enum class ParallelPassExecutionStatus : std::uint8_t {
  Completed,
  SerialFallback,
  FailStop,
};

enum class ParallelPassFailurePhase : std::uint8_t {
  None,
  CompletionOrderValidation,
  ParentPreparation,
  ChildCreation,
  BeginPassActions,
  LogicalCommandReplay,
  ChildEmission,
  ChildEnd,
  ChildJoin,
  EndPassActions,
  ParentEnd,
  SidecarPublication,
  CompletionPublication,
};

struct ParallelPassExecutionResult {
  ParallelPassExecutionStatus status =
      ParallelPassExecutionStatus::SerialFallback;
  ParallelPassFallbackReason fallback =
      ParallelPassFallbackReason::NotRequested;
  ParallelPassFailurePhase failurePhase = ParallelPassFailurePhase::None;
  std::uint32_t affectedChild = 0;
  bool crossedEffectBoundary = false;
};

// Backend is a deliberately small seam used by deterministic native fakes now
// and by a future WMT parent/child implementation only after it exists. Child
// creation is ordered and pre-effect. beginPassActions() is the effect edge;
// every failure at or after it is fail-stop because encoded Metal work cannot
// be rewound into the serial lane.
template <typename Backend>
ParallelPassExecutionResult executeParallelRenderPass(
    std::span<const ParallelPassChildPlan> children,
    std::span<const std::uint32_t> completionOrder,
    Backend& backend) noexcept {
  ParallelPassExecutionResult result{};
  if (children.size() < 2u ||
      children.size() > kParallelRenderPassChildCapacity ||
      completionOrder.size() != children.size()) {
    result.fallback = ParallelPassFallbackReason::InvalidCompletionOrder;
    result.failurePhase = ParallelPassFailurePhase::CompletionOrderValidation;
    return result;
  }
  std::array<bool, kParallelRenderPassChildCapacity> seen{};
  for (const auto ordinal : completionOrder) {
    if (ordinal >= children.size() || seen[ordinal]) {
      result.fallback = ParallelPassFallbackReason::InvalidCompletionOrder;
      result.failurePhase =
          ParallelPassFailurePhase::CompletionOrderValidation;
      return result;
    }
    seen[ordinal] = true;
  }
  if (!backend.prepareParent()) {
    result.fallback = ParallelPassFallbackReason::ParentPreparationFailed;
    result.failurePhase = ParallelPassFailurePhase::ParentPreparation;
    return result;
  }
  for (const auto& child : children) {
    if (!backend.createChild(child)) {
      backend.abandonPrepared();
      result.fallback = ParallelPassFallbackReason::ChildCreationFailed;
      result.failurePhase = ParallelPassFailurePhase::ChildCreation;
      result.affectedChild = child.childOrdinal;
      return result;
    }
  }

  result.crossedEffectBoundary = true;
  result.status = ParallelPassExecutionStatus::FailStop;
  result.fallback = ParallelPassFallbackReason::None;
  if (!backend.beginPassActions()) {
    result.failurePhase = ParallelPassFailurePhase::BeginPassActions;
    return result;
  }
  if (!backend.replayLogicalCommands(children)) {
    result.failurePhase = ParallelPassFailurePhase::LogicalCommandReplay;
    return result;
  }
  for (const auto& child : children) {
    if (!backend.emitChild(child)) {
      result.failurePhase = ParallelPassFailurePhase::ChildEmission;
      result.affectedChild = child.childOrdinal;
      return result;
    }
    if (!backend.endChild(child.childOrdinal)) {
      result.failurePhase = ParallelPassFailurePhase::ChildEnd;
      result.affectedChild = child.childOrdinal;
      return result;
    }
  }
  for (const auto ordinal : completionOrder) {
    if (!backend.joinChild(ordinal)) {
      result.failurePhase = ParallelPassFailurePhase::ChildJoin;
      result.affectedChild = ordinal;
      return result;
    }
  }
  if (!backend.endPassActions()) {
    result.failurePhase = ParallelPassFailurePhase::EndPassActions;
    return result;
  }
  if (!backend.endParent()) {
    result.failurePhase = ParallelPassFailurePhase::ParentEnd;
    return result;
  }
  if (!backend.publishSidecars()) {
    result.failurePhase = ParallelPassFailurePhase::SidecarPublication;
    return result;
  }
  if (!backend.publishCompletion()) {
    result.failurePhase = ParallelPassFailurePhase::CompletionPublication;
    return result;
  }
  result.status = ParallelPassExecutionStatus::Completed;
  result.failurePhase = ParallelPassFailurePhase::None;
  return result;
}

static_assert(std::is_trivially_copyable_v<ParallelFirstDrawSnapshot>);
static_assert(std::is_standard_layout_v<ParallelPassChildPlan>);
static_assert(std::is_trivially_copyable_v<ParallelPassPlanStorage>);

}  // namespace dxmt9::encoders
