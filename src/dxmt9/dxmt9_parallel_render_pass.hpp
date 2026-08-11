#pragma once

#include "dxmt9_encode_partition.hpp"
#include "dxmt9_source_semantics.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>
#include <utility>

namespace dxmt9::encoders {

inline constexpr std::size_t kParallelRenderPassChildCapacity = 16u;
inline constexpr std::size_t kParallelRenderPassCandidateCapacity = 16u;

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
  NonChildDrawRun,
  TooFewChildren,
  ChildCapacity,
  PassCapacity,
  AttachmentMismatch,
  ResourceSetIncomplete,
  ResourceIdentityProof,
  ResourceHazard,
  RenderRoute,
  PassActionEpoch,
  QueryState,
  UpdateTextureState,
  CaptureState,
  InitializerState,
  OrderedControlState,
  SidecarState,
  FirstDrawSnapshot,
  Count,
};

// These callbacks are borrowed only for the synchronous producer call. The
// snapshot owns only their resolved value results; neither context may escape.
// A resolver is the proof-producing owner: raw handles are never relabeled as
// canonical merely because a caller supplies a boolean.
struct ParallelPassResourceIdentityProof {
  const void* context = nullptr;
  bool (*resolve)(const void* context, std::uint64_t raw,
                  std::uint64_t& canonical) noexcept = nullptr;

  constexpr bool valid() const noexcept { return resolve != nullptr; }
};

struct ParallelPassRenderRouteProof {
  const void* context = nullptr;
  core::RenderRoute (*resolve)(const void* context,
                               core::FlatDrawStateView state) noexcept = nullptr;

  constexpr bool valid() const noexcept { return resolve != nullptr; }
};

enum ParallelPassCoordinatorProofFlag : std::uint32_t {
  ParallelPassQueryAbsent = 1u << 0,
  ParallelPassUpdateTextureAbsent = 1u << 1,
  ParallelPassCaptureInactive = 1u << 2,
  ParallelPassInitializerIndependent = 1u << 3,
  ParallelPassOrderedControlAbsent = 1u << 4,
  ParallelPassSidecarObservationAbsent = 1u << 5,
};

inline constexpr std::uint32_t kParallelPassCoordinatorProofComplete =
    ParallelPassQueryAbsent | ParallelPassUpdateTextureAbsent |
    ParallelPassCaptureInactive | ParallelPassInitializerIndependent |
    ParallelPassOrderedControlAbsent | ParallelPassSidecarObservationAbsent;

struct ParallelPassCoordinatorProof {
  std::uint64_t firstPassActionEpoch = 0;
  std::uint32_t flags = 0;

  constexpr bool proves(ParallelPassCoordinatorProofFlag fact) const noexcept {
    return (flags & static_cast<std::uint32_t>(fact)) != 0;
  }
};

struct ParallelPassStaticProofInput {
  ParallelPassResourceIdentityProof resources{};
  ParallelPassRenderRouteProof route{};
  ParallelPassCoordinatorProof coordinator{};
};

struct SealedParallelPassSnapshotInput {
  const EncodePartitionReplayStream* stream = nullptr;
  std::span<const EncodePartitionRangeSnapshot> ranges{};
  ParallelPassStaticProofInput proofs{};
  bool planValidated = false;
  bool sourceStartsPass = false;
  bool sourceEndsPass = false;
};

struct ParallelPassCommandLocator {
  RetainedEncodeSourceLocator source{};
  std::uint32_t replayOrdinal = 0;
  std::uint32_t commandIndex = 0;
  core::MetalCommandKind kind = core::MetalCommandKind::DrawRun;
  bool valid = false;

  friend constexpr bool operator==(const ParallelPassCommandLocator&,
                                   const ParallelPassCommandLocator&) = default;
};

// Fixed value-owned shadow of one complete source-local logical pass. It owns
// no SourcePayloadView, resolved span, page, Metal object, or mutable native
// state. A future executor must re-resolve every locator under one residency
// pin and validate native state plus passActionEpoch before child creation.
struct SealedParallelPassSnapshot {
  std::array<EncodePartitionRangeSnapshot,
             kParallelRenderPassChildCapacity> ranges{};
  std::array<ParallelFirstDrawSnapshot,
             kParallelRenderPassChildCapacity> firstDraws{};
  core::RenderAttachmentKey attachments{};
  core::ExactResourceSet attachmentWrites{};
  core::ExactResourceSet resourceReads{};
  core::CpuReadyTape::SourceRef source{};
  ParallelPassCommandLocator firstDraw{};
  ParallelPassCommandLocator leadingClear{};
  ParallelPassCommandLocator sealingCommand{};
  std::uint64_t seqId = 0;
  std::uint64_t passActionEpoch = 0;
  std::uint64_t drawCount = 0;
  std::uint32_t replayOrdinalBegin = 0;
  std::uint32_t replayOrdinalEnd = 0;
  std::uint32_t childCount = 0;
  bool sealedAtSourceEnd = false;

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

struct SealedParallelPassSnapshotBatch {
  std::array<SealedParallelPassSnapshot,
             kParallelRenderPassCandidateCapacity> passes{};
  std::size_t count = 0;

  void reset() noexcept { count = 0; }
  std::span<const SealedParallelPassSnapshot> view() const noexcept {
    return std::span<const SealedParallelPassSnapshot>(passes.data(), count);
  }
};

struct SealedParallelPassSnapshotBatchResult {
  SealedParallelPassSnapshotFallback fallback =
      SealedParallelPassSnapshotFallback::PlanMissing;
  std::array<std::uint32_t,
             static_cast<std::size_t>(SealedParallelPassSnapshotFallback::Count)>
      rejectionCounts{};
  std::uint64_t drawCount = 0;
  std::uint32_t childCount = 0;
  std::uint32_t candidateCount = 0;
  std::uint32_t sealedCount = 0;
  std::uint32_t eligibleCount = 0;
  std::uint32_t eligibleCountMax = 0;
  std::uint32_t childCountMax = 0;
  std::uint64_t drawCountMax = 0;
  bool considered = false;

  friend constexpr bool operator==(
      const SealedParallelPassSnapshotBatchResult&,
      const SealedParallelPassSnapshotBatchResult&) = default;
};

// Allocation-free producer over the final effective replay order. It can emit
// several independent complete pass observations from one source, while
// leaving every coordinator command outside child ranges. Full production-plan
// validation must already have succeeded; the function has no side effects.
SealedParallelPassSnapshotBatchResult produceSealedParallelPassSnapshots(
    const SealedParallelPassSnapshotInput& input,
    SealedParallelPassSnapshotBatch& snapshots) noexcept;

template <typename Producer>
bool runParallelPassObservationIfEnabled(bool explicitParallel,
                                         bool perfEnabled,
                                         Producer&& producer) {
  if (!explicitParallel || !perfEnabled) {
    return false;
  }
  std::forward<Producer>(producer)();
  return true;
}

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
  case SealedParallelPassSnapshotFallback::NonChildDrawRun:
    return ParallelPassFallbackReason::CoordinatorCommand;
  case SealedParallelPassSnapshotFallback::TooFewChildren:
    return ParallelPassFallbackReason::TooFewChildren;
  case SealedParallelPassSnapshotFallback::ChildCapacity:
  case SealedParallelPassSnapshotFallback::PassCapacity:
    return ParallelPassFallbackReason::ChildCapacity;
  case SealedParallelPassSnapshotFallback::AttachmentMismatch:
  case SealedParallelPassSnapshotFallback::ResourceSetIncomplete:
  case SealedParallelPassSnapshotFallback::ResourceIdentityProof:
  case SealedParallelPassSnapshotFallback::ResourceHazard:
  case SealedParallelPassSnapshotFallback::RenderRoute:
  case SealedParallelPassSnapshotFallback::PassActionEpoch:
  case SealedParallelPassSnapshotFallback::QueryState:
  case SealedParallelPassSnapshotFallback::UpdateTextureState:
  case SealedParallelPassSnapshotFallback::CaptureState:
  case SealedParallelPassSnapshotFallback::InitializerState:
  case SealedParallelPassSnapshotFallback::OrderedControlState:
  case SealedParallelPassSnapshotFallback::SidecarState:
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
  std::uint64_t passActionEpoch = 0;
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
  if (input.passActionEpoch == 0u) {
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
        !input.firstDrawSnapshots[i].entryRender.entryStateComplete() ||
        input.firstDrawSnapshots[i].entryRender.route ==
            core::RenderRoute::Unknown ||
        !input.firstDrawSnapshots[i].entryRender.entryReads.complete() ||
        !input.firstDrawSnapshots[i].entryRender.entryReads.canonicalized() ||
        input.firstDrawSnapshots[i].entryRender.passActionEpoch !=
            input.passActionEpoch) {
      return reject(ParallelPassFallbackReason::FirstDrawSnapshotMissing);
    }
    if (i != 0u && input.firstDrawSnapshots[i].entryRender.route !=
                       input.firstDrawSnapshots[0].entryRender.route) {
      return reject(ParallelPassFallbackReason::UnresolvedHazard);
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
  const std::uint64_t passActionEpoch =
      children.front().firstDraw.entryRender.passActionEpoch;
  const core::RenderRoute route =
      children.front().firstDraw.entryRender.route;
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
        !child.firstDraw.entryRender.entryStateComplete() ||
        route == core::RenderRoute::Unknown ||
        child.firstDraw.entryRender.route != route ||
        !child.firstDraw.entryRender.entryReads.complete() ||
        !child.firstDraw.entryRender.entryReads.canonicalized() ||
        passActionEpoch == 0u ||
        child.firstDraw.entryRender.passActionEpoch != passActionEpoch) {
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
static_assert(std::is_trivially_copyable_v<ParallelPassCommandLocator>);
static_assert(std::is_trivially_copyable_v<SealedParallelPassSnapshot>);
static_assert(std::is_standard_layout_v<SealedParallelPassSnapshot>);
static_assert(std::is_trivially_copyable_v<SealedParallelPassSnapshotBatch>);
static_assert(std::is_standard_layout_v<ParallelPassChildPlan>);
static_assert(std::is_trivially_copyable_v<ParallelPassPlanStorage>);

}  // namespace dxmt9::encoders
