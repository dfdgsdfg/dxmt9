#pragma once

#include "dxmt9_encode_partition.hpp"
#include "dxmt9_source_semantics.hpp"

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
  EncodePartitionEntrySnapshot provenance{};
  core::RenderContinuationKey entryRender{};
  std::uint64_t generation = 0;
  bool complete = false;

  friend constexpr bool operator==(const ParallelFirstDrawSnapshot&,
                                   const ParallelFirstDrawSnapshot&) = default;
};

enum class SealedParallelPassSnapshotFallback : std::uint8_t {
  None,
  PlanMissing,
  PlanNotValidated,
  ReplayInvalid,
  UnsealedStart,
  UnsealedEnd,
  CoordinatorCommand,
  TooFewChildren,
  ChildCapacity,
  AttachmentMismatch,
  ResourceHazard,
  FirstDrawSnapshot,
  Count,
};

struct SealedParallelPassSnapshotInput {
  const EncodePartitionReplayStream* stream = nullptr;
  std::span<const EncodePartitionRangeSnapshot> ranges{};
  bool planValidated = false;
  bool sourceStartsPass = false;
  bool sourceEndsPass = false;
};

// Fixed value-owned shadow of one complete single-source logical pass. It
// deliberately contains no SourcePayloadView, resolved span, Metal object, or
// page pointer. The source locator must be re-resolved under a residency pin by
// any future executor before child creation.
struct SealedParallelPassSnapshot {
  std::array<EncodePartitionRangeSnapshot,
             kParallelRenderPassChildCapacity> ranges{};
  std::array<ParallelFirstDrawSnapshot,
             kParallelRenderPassChildCapacity> firstDraws{};
  core::RenderAttachmentKey attachments{};
  core::ExactResourceSet attachmentWrites{};
  core::CpuReadyTape::SourceRef source{};
  std::uint64_t seqId = 0;
  std::uint64_t drawCount = 0;
  std::uint32_t childCount = 0;
  bool hasTerminalPresent = false;

  void reset() noexcept { *this = {}; }
  std::span<const EncodePartitionRangeSnapshot> rangeView() const noexcept {
    return std::span<const EncodePartitionRangeSnapshot>(ranges.data(),
                                                          childCount);
  }
  std::span<const ParallelFirstDrawSnapshot> firstDrawView() const noexcept {
    return std::span<const ParallelFirstDrawSnapshot>(firstDraws.data(),
                                                       childCount);
  }
};

struct SealedParallelPassSnapshotResult {
  SealedParallelPassSnapshotFallback fallback =
      SealedParallelPassSnapshotFallback::PlanMissing;
  std::uint64_t drawCount = 0;
  std::uint32_t childCount = 0;
  bool considered = false;
  bool sealed = false;
  bool eligible = false;

  friend constexpr bool operator==(
      const SealedParallelPassSnapshotResult&,
      const SealedParallelPassSnapshotResult&) = default;
};

// Production shadow producer for the first safely provable shape: a fresh,
// source-local pass whose selected stream contains only DrawRuns and an
// optional final Present. Full-plan validation must already have succeeded.
// The function is allocation-free and performs no Metal or queue side effect.
SealedParallelPassSnapshotResult produceSealedParallelPassSnapshot(
    const SealedParallelPassSnapshotInput& input,
    SealedParallelPassSnapshot& snapshot) noexcept;

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
  ChildRangeInvalid,
  ChildRangeOrderInvalid,
  ChildOrdinalInvalid,
  LocalShadowInvalid,
  FirstDrawProvenanceInvalid,
  FullFirstDrawBindingRequired,
  Count,
};

inline ParallelPassFallbackReason parallelPassFallbackForSnapshot(
    SealedParallelPassSnapshotFallback fallback) noexcept {
  switch (fallback) {
  case SealedParallelPassSnapshotFallback::None:
    return ParallelPassFallbackReason::None;
  case SealedParallelPassSnapshotFallback::PlanMissing:
    return ParallelPassFallbackReason::NoExplicitPlan;
  case SealedParallelPassSnapshotFallback::PlanNotValidated:
  case SealedParallelPassSnapshotFallback::ReplayInvalid:
    return ParallelPassFallbackReason::PlanNotValidated;
  case SealedParallelPassSnapshotFallback::UnsealedStart:
  case SealedParallelPassSnapshotFallback::UnsealedEnd:
    return ParallelPassFallbackReason::PassNotSealed;
  case SealedParallelPassSnapshotFallback::CoordinatorCommand:
    return ParallelPassFallbackReason::CoordinatorCommand;
  case SealedParallelPassSnapshotFallback::TooFewChildren:
    return ParallelPassFallbackReason::TooFewChildren;
  case SealedParallelPassSnapshotFallback::ChildCapacity:
    return ParallelPassFallbackReason::ChildCapacity;
  case SealedParallelPassSnapshotFallback::AttachmentMismatch:
  case SealedParallelPassSnapshotFallback::ResourceHazard:
    return ParallelPassFallbackReason::UnresolvedHazard;
  case SealedParallelPassSnapshotFallback::FirstDrawSnapshot:
  case SealedParallelPassSnapshotFallback::Count:
    return ParallelPassFallbackReason::FirstDrawSnapshotMissing;
  }
  return ParallelPassFallbackReason::FirstDrawSnapshotMissing;
}

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
        input.firstDrawSnapshots[i].generation == 0u ||
        !input.firstDrawSnapshots[i].entryRender.valid() ||
        !input.firstDrawSnapshots[i].entryRender.entryStateComplete()) {
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
  ChildPlanValidation,
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

inline ParallelPassFallbackReason validateParallelPassChildPlans(
    std::span<const ParallelPassChildPlan> children) noexcept {
  if (children.size() < 2u ||
      children.size() > kParallelRenderPassChildCapacity) {
    return ParallelPassFallbackReason::ChildCapacity;
  }
  std::array<bool, kParallelRenderPassChildCapacity + 1u> shadows{};
  const auto& firstSource = children.front().range.entry.source;
  std::uint32_t previousReplayOrdinal = 0u;
  std::uint32_t previousDrawEnd = 0u;
  std::uint32_t previousCommandIndex = 0u;
  for (std::size_t i = 0; i < children.size(); ++i) {
    const auto& child = children[i];
    const auto& range = child.range;
    if (range.kind != EncodePartitionRangeKind::DrawRunEntries ||
        range.replayOrdinalCount != 1u || range.drawEntryCount == 0u ||
        !range.entry.source.tapeSource.valid() ||
        range.entry.source.seqId == 0u ||
        range.entry.drawParamIndex >
            UINT32_MAX - range.drawEntryCount) {
      return ParallelPassFallbackReason::ChildRangeInvalid;
    }
    if (range.entry.source != firstSource) {
      return ParallelPassFallbackReason::ChildRangeOrderInvalid;
    }
    if (child.childOrdinal != i) {
      return ParallelPassFallbackReason::ChildOrdinalInvalid;
    }
    if (child.localShadowOrdinal == 0u ||
        child.localShadowOrdinal > kParallelRenderPassChildCapacity ||
        shadows[child.localShadowOrdinal]) {
      return ParallelPassFallbackReason::LocalShadowInvalid;
    }
    shadows[child.localShadowOrdinal] = true;
    if (!child.firstDraw.complete || child.firstDraw.generation == 0u ||
        child.firstDraw.provenance != range.entry ||
        !child.firstDraw.entryRender.valid() ||
        !child.firstDraw.entryRender.entryStateComplete()) {
      return ParallelPassFallbackReason::FirstDrawProvenanceInvalid;
    }
    if (!child.forceFullFirstDrawBinding) {
      return ParallelPassFallbackReason::FullFirstDrawBindingRequired;
    }
    if (i != 0u) {
      if (range.replayOrdinalBegin < previousReplayOrdinal) {
        return ParallelPassFallbackReason::ChildRangeOrderInvalid;
      }
      if (range.replayOrdinalBegin == previousReplayOrdinal) {
        if (range.entry.commandIndex != previousCommandIndex ||
            range.entry.drawParamIndex != previousDrawEnd) {
          return ParallelPassFallbackReason::ChildRangeOrderInvalid;
        }
      } else if (range.replayOrdinalBegin != previousReplayOrdinal + 1u ||
                 range.entry.commandIndex == previousCommandIndex) {
        return ParallelPassFallbackReason::ChildRangeOrderInvalid;
      }
    }
    previousReplayOrdinal = range.replayOrdinalBegin;
    previousCommandIndex = range.entry.commandIndex;
    previousDrawEnd = range.entry.drawParamIndex + range.drawEntryCount;
  }
  return ParallelPassFallbackReason::None;
}

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
  const auto planValidation = validateParallelPassChildPlans(children);
  if (planValidation != ParallelPassFallbackReason::None) {
    result.fallback = planValidation;
    result.failurePhase = ParallelPassFailurePhase::ChildPlanValidation;
    return result;
  }
  if (completionOrder.size() != children.size()) {
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
  const auto failStop = [&](ParallelPassFailurePhase phase,
                            std::uint32_t child = 0u) {
    result.failurePhase = phase;
    result.affectedChild = child;
    backend.failStop(phase, child);
    return result;
  };
  if (!backend.beginPassActions()) {
    return failStop(ParallelPassFailurePhase::BeginPassActions);
  }
  if (!backend.replayLogicalCommands(children)) {
    return failStop(ParallelPassFailurePhase::LogicalCommandReplay);
  }
  for (const auto& child : children) {
    if (!backend.emitChild(child)) {
      return failStop(ParallelPassFailurePhase::ChildEmission,
                      child.childOrdinal);
    }
    if (!backend.endChild(child.childOrdinal)) {
      return failStop(ParallelPassFailurePhase::ChildEnd,
                      child.childOrdinal);
    }
  }
  for (const auto ordinal : completionOrder) {
    if (!backend.joinChild(ordinal)) {
      return failStop(ParallelPassFailurePhase::ChildJoin, ordinal);
    }
  }
  if (!backend.endPassActions()) {
    return failStop(ParallelPassFailurePhase::EndPassActions);
  }
  if (!backend.endParent()) {
    return failStop(ParallelPassFailurePhase::ParentEnd);
  }
  if (!backend.publishSidecars()) {
    return failStop(ParallelPassFailurePhase::SidecarPublication);
  }
  if (!backend.publishCompletion()) {
    return failStop(ParallelPassFailurePhase::CompletionPublication);
  }
  result.status = ParallelPassExecutionStatus::Completed;
  result.failurePhase = ParallelPassFailurePhase::None;
  return result;
}

static_assert(std::is_trivially_copyable_v<ParallelFirstDrawSnapshot>);
static_assert(std::is_trivially_copyable_v<SealedParallelPassSnapshot>);
static_assert(std::is_standard_layout_v<SealedParallelPassSnapshot>);
static_assert(std::is_standard_layout_v<ParallelPassChildPlan>);
static_assert(std::is_trivially_copyable_v<ParallelPassPlanStorage>);

}  // namespace dxmt9::encoders
