#pragma once

#include "dxmt9_encode_partition.hpp"
#include "dxmt9_source_semantics.hpp"
#include "dxmt9_uniform_dirty.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>
#include <utility>

namespace dxmt9::encoders {

inline constexpr std::size_t kParallelRenderPassChildCapacity = 16u;
inline constexpr std::size_t kParallelRenderPassCandidateCapacity = 16u;
inline constexpr std::uint32_t kParallelRenderPassNoFailedChild = UINT32_MAX;

enum class ParallelPassDirectBindingMode : std::uint8_t {
  Stage1Direct,
  Stage2DirectCbuf,
  Count,
};

enum class ParallelPassBindingRejectReason : std::uint8_t {
  None,
  MissingPso,
  Stage2ArgumentTable,
  ResourceArray,
  MixedAbi,
  OverrideRebuild,
  Count,
};

struct ParallelPassBindingKeyInput {
  bool psoPresent = false;
  bool argbufHybrid = false;
  bool argbufResourceArray = false;
  bool argbufDirectCbuf = false;
  bool overrideRebuild = false;
};

struct ParallelPassBindingKeyDecision {
  ParallelPassDirectBindingMode mode =
      ParallelPassDirectBindingMode::Stage1Direct;
  ParallelPassBindingRejectReason reject =
      ParallelPassBindingRejectReason::MissingPso;

  constexpr bool accepted() const noexcept {
    return reject == ParallelPassBindingRejectReason::None;
  }
};

// Pure pre-effect classifier for the only two child-local binding ABIs. The
// caller separately proves that every command in the pass selects the same
// accepted mode.
inline constexpr ParallelPassBindingKeyDecision
classifyParallelPassBindingKey(
    const ParallelPassBindingKeyInput& input) noexcept {
  if (!input.psoPresent) {
    return {};
  }
  if (input.argbufResourceArray) {
    return {
        .reject = ParallelPassBindingRejectReason::ResourceArray,
    };
  }
  if (input.overrideRebuild) {
    return {
        .reject = ParallelPassBindingRejectReason::OverrideRebuild,
    };
  }
  if (input.argbufHybrid && !input.argbufDirectCbuf) {
    return {
        .reject = ParallelPassBindingRejectReason::Stage2ArgumentTable,
    };
  }
  if (!input.argbufHybrid && input.argbufDirectCbuf) {
    return {
        .reject = ParallelPassBindingRejectReason::MixedAbi,
    };
  }
  return {
      .mode = input.argbufHybrid
          ? ParallelPassDirectBindingMode::Stage2DirectCbuf
          : ParallelPassDirectBindingMode::Stage1Direct,
      .reject = ParallelPassBindingRejectReason::None,
  };
}

struct ParallelPassBindingSnapshot {
  core::PsoHandle firstRenderPso{};
  uniform::DrawBindingPayloadIdentity firstPayload{};
  uniform::DirectCbufPayloadCounts firstPayloadCounts{};
  ParallelPassDirectBindingMode mode =
      ParallelPassDirectBindingMode::Stage1Direct;
  ParallelPassBindingRejectReason reject =
      ParallelPassBindingRejectReason::MissingPso;
  bool complete = false;
};

enum class ParallelPassEconomicsRejectReason : std::uint8_t {
  None,
  ForcedStage1,
  ThinChild,
  UnbalancedChild,
  PsoFirstBindAmplification,
  UniformFirstBindAmplification,
  InvalidOrOverflow,
  Count,
};

struct ParallelPassEconomicsSummary {
  std::uint64_t totalDraws = 0;
  std::uint64_t stage1Draws = 0;
  std::uint64_t stage2bDraws = 0;
  std::uint64_t forcedStage1Draws = 0;
  std::uint64_t psoBoundaryTransitions = 0;
  std::uint64_t uniformBoundaryTransitions = 0;
  std::uint32_t childCount = 0;
  std::uint32_t minimumChildDraws = 0;
  std::uint32_t maximumChildDraws = 0;
  bool valid = false;
  bool overflow = false;

  friend constexpr bool operator==(const ParallelPassEconomicsSummary&,
                                   const ParallelPassEconomicsSummary&) =
      default;
};

struct ParallelPassEconomicsDecision {
  ParallelPassEconomicsRejectReason reject =
      ParallelPassEconomicsRejectReason::InvalidOrOverflow;
  bool considered = false;
  bool accepted = false;

  friend constexpr bool operator==(const ParallelPassEconomicsDecision&,
                                   const ParallelPassEconomicsDecision&) =
      default;
};

struct ParallelPassEconomicsAccounting {
  std::array<std::uint64_t,
             static_cast<std::size_t>(ParallelPassEconomicsRejectReason::Count)>
      rejectionCounts{};
  std::uint64_t considered = 0;
  std::uint64_t accepted = 0;
  std::uint64_t serialFallback = 0;

  constexpr bool conserves() const noexcept {
    std::uint64_t reasons = 0;
    for (std::size_t i = 1u; i < rejectionCounts.size(); ++i) {
      reasons += rejectionCounts[i];
    }
    return considered == accepted + serialFallback &&
        serialFallback == reasons;
  }
};

struct ParallelPassChildSubdivision {
  std::array<std::uint32_t, kParallelRenderPassChildCapacity> drawBegins{};
  std::array<std::uint32_t, kParallelRenderPassChildCapacity> drawCounts{};
  std::uint32_t childCount = 0;
  bool valid = false;
};

enum class ParallelPassWholeCommandPlanFailure : std::uint8_t {
  None,
  NoTwoChildWork,
  InvalidOrOverflow,
};

struct ParallelPassWholeCommandSubdivision {
  std::array<std::uint32_t, kParallelRenderPassChildCapacity>
      replayOrdinalBegins{};
  std::array<std::uint32_t, kParallelRenderPassChildCapacity>
      replayOrdinalCounts{};
  std::array<std::uint64_t, kParallelRenderPassChildCapacity> drawCounts{};
  ParallelPassWholeCommandPlanFailure failure =
      ParallelPassWholeCommandPlanFailure::InvalidOrOverflow;
  std::uint32_t childCount = 0;

  constexpr bool valid() const noexcept {
    return failure == ParallelPassWholeCommandPlanFailure::None &&
        childCount >= 2u;
  }
};

// Allocation-free ordered grouping for indivisible DrawRun commands. Each
// non-final child closes at the earliest checked prefix of at least 64 draws
// only when a complete >=64 suffix remains. This absorbs a thin final suffix
// into its preceding child and caps emitted groups at sixteen.
template <typename DrawCountAt>
inline constexpr ParallelPassWholeCommandSubdivision
subdivideParallelPassWholeCommands(std::uint32_t replayOrdinalBegin,
                                   std::uint32_t commandCount,
                                   std::uint64_t totalDraws,
                                   DrawCountAt&& drawCountAt) noexcept {
  ParallelPassWholeCommandSubdivision result{};
  if (commandCount < 2u ||
      totalDraws < 2u * kProductionPartitionDrawThreshold) {
    result.failure = ParallelPassWholeCommandPlanFailure::NoTwoChildWork;
    return result;
  }
  const std::uint32_t maximumChildren = static_cast<std::uint32_t>(
      std::min<std::uint64_t>({
          kParallelRenderPassChildCapacity, commandCount,
          totalDraws / kProductionPartitionDrawThreshold}));
  if (maximumChildren < 2u ||
      replayOrdinalBegin > UINT32_MAX - commandCount) {
    result.failure = ParallelPassWholeCommandPlanFailure::InvalidOrOverflow;
    return result;
  }

  std::uint32_t groupBegin = replayOrdinalBegin;
  std::uint32_t groupCommands = 0u;
  std::uint64_t groupDraws = 0u;
  std::uint64_t consumedDraws = 0u;
  for (std::uint32_t command = 0u; command < commandCount; ++command) {
    const std::uint64_t commandDraws = drawCountAt(command);
    if (commandDraws == 0u || groupDraws > UINT64_MAX - commandDraws ||
        commandDraws > totalDraws - consumedDraws) {
      result.failure = ParallelPassWholeCommandPlanFailure::InvalidOrOverflow;
      return result;
    }
    groupDraws += commandDraws;
    consumedDraws += commandDraws;
    ++groupCommands;
    if (consumedDraws > totalDraws) {
      result.failure = ParallelPassWholeCommandPlanFailure::InvalidOrOverflow;
      return result;
    }
    const std::uint64_t remainingDraws = totalDraws - consumedDraws;
    const std::uint32_t remainingCommands = commandCount - command - 1u;
    if (result.childCount + 1u < maximumChildren &&
        groupDraws >= kProductionPartitionDrawThreshold &&
        remainingDraws >= kProductionPartitionDrawThreshold &&
        remainingCommands != 0u) {
      const std::size_t child = result.childCount++;
      result.replayOrdinalBegins[child] = groupBegin;
      result.replayOrdinalCounts[child] = groupCommands;
      result.drawCounts[child] = groupDraws;
      groupBegin += groupCommands;
      groupCommands = 0u;
      groupDraws = 0u;
    }
  }
  if (consumedDraws != totalDraws || groupCommands == 0u ||
      groupDraws < kProductionPartitionDrawThreshold ||
      result.childCount >= kParallelRenderPassChildCapacity) {
    result.failure = consumedDraws == totalDraws
        ? ParallelPassWholeCommandPlanFailure::NoTwoChildWork
        : ParallelPassWholeCommandPlanFailure::InvalidOrOverflow;
    return result;
  }
  const std::size_t finalChild = result.childCount++;
  result.replayOrdinalBegins[finalChild] = groupBegin;
  result.replayOrdinalCounts[finalChild] = groupCommands;
  result.drawCounts[finalChild] = groupDraws;
  if (result.childCount < 2u) {
    result.failure = ParallelPassWholeCommandPlanFailure::NoTwoChildWork;
    return result;
  }
  result.failure = ParallelPassWholeCommandPlanFailure::None;
  return result;
}

// ExplicitParallel-only subdivision. The serial production partitioner keeps
// its independent 32-draw target; sealed passes instead derive no more than
// floor(total/64) even children and place any remainder in the final children.
inline constexpr ParallelPassChildSubdivision
subdivideParallelPassDraws(std::uint64_t totalDraws) noexcept {
  ParallelPassChildSubdivision result{};
  if (totalDraws > UINT32_MAX) {
    return result;
  }
  const std::uint64_t boundedChildren = std::min<std::uint64_t>(
      kParallelRenderPassChildCapacity,
      totalDraws / kProductionPartitionDrawThreshold);
  if (boundedChildren < 2u) {
    return result;
  }
  result.childCount = static_cast<std::uint32_t>(boundedChildren);
  const std::uint32_t draws = static_cast<std::uint32_t>(totalDraws);
  const std::uint32_t quotient = draws / result.childCount;
  const std::uint32_t remainder = draws % result.childCount;
  std::uint32_t cursor = 0u;
  for (std::uint32_t child = 0u; child < result.childCount; ++child) {
    const std::uint32_t count = quotient +
        (child >= result.childCount - remainder ? 1u : 0u);
    if (count < kProductionPartitionDrawThreshold ||
        cursor > draws - count) {
      return {};
    }
    result.drawBegins[child] = cursor;
    result.drawCounts[child] = count;
    cursor += count;
  }
  result.valid = cursor == draws;
  return result.valid ? result : ParallelPassChildSubdivision{};
}

// Enforced economics policy. It reuses the production planner's
// existing 64-draw eligibility quantum, bounds actual child imbalance by that
// quantum, and admits an extra child first bind only when the corresponding
// child boundary already changes both PSO and uniform identity in serial order.
inline constexpr ParallelPassEconomicsDecision
classifyParallelPassEconomics(
    const ParallelPassEconomicsSummary& summary) noexcept {
  ParallelPassEconomicsDecision result{.considered = true};
  auto reject = [&](ParallelPassEconomicsRejectReason reason) {
    result.reject = reason;
    return result;
  };
  const bool drawCountsConserve =
      summary.stage1Draws <= summary.totalDraws &&
      summary.stage2bDraws <= summary.totalDraws - summary.stage1Draws &&
      summary.forcedStage1Draws ==
          summary.totalDraws - summary.stage1Draws - summary.stage2bDraws;
  const std::uint64_t minimumCoveredDraws =
      static_cast<std::uint64_t>(summary.childCount) *
      summary.minimumChildDraws;
  const std::uint64_t remainingChildren = summary.childCount - 1u;
  const std::uint64_t minimumWithMaximum =
      summary.maximumChildDraws +
      remainingChildren * summary.minimumChildDraws;
  const std::uint64_t maximumWithMinimum =
      summary.minimumChildDraws +
      remainingChildren * summary.maximumChildDraws;
  if (!summary.valid || summary.overflow || summary.childCount < 2u ||
      summary.childCount > kParallelRenderPassChildCapacity ||
      summary.minimumChildDraws == 0u ||
      summary.maximumChildDraws < summary.minimumChildDraws ||
      summary.maximumChildDraws > summary.totalDraws ||
      summary.totalDraws < minimumCoveredDraws ||
      summary.totalDraws < minimumWithMaximum ||
      summary.totalDraws > maximumWithMinimum || !drawCountsConserve) {
    return reject(ParallelPassEconomicsRejectReason::InvalidOrOverflow);
  }
  if (summary.forcedStage1Draws != 0u) {
    return reject(ParallelPassEconomicsRejectReason::ForcedStage1);
  }
  if (summary.minimumChildDraws < kProductionPartitionDrawThreshold) {
    return reject(ParallelPassEconomicsRejectReason::ThinChild);
  }
  if (summary.maximumChildDraws - summary.minimumChildDraws >
      kProductionPartitionDrawThreshold) {
    return reject(ParallelPassEconomicsRejectReason::UnbalancedChild);
  }
  const std::uint64_t extraChildFirstBinds = summary.childCount - 1u;
  if (summary.psoBoundaryTransitions > extraChildFirstBinds ||
      summary.uniformBoundaryTransitions > extraChildFirstBinds) {
    return reject(ParallelPassEconomicsRejectReason::InvalidOrOverflow);
  }
  if (extraChildFirstBinds > summary.psoBoundaryTransitions) {
    return reject(
        ParallelPassEconomicsRejectReason::PsoFirstBindAmplification);
  }
  if (extraChildFirstBinds > summary.uniformBoundaryTransitions) {
    return reject(
        ParallelPassEconomicsRejectReason::UniformFirstBindAmplification);
  }
  result.reject = ParallelPassEconomicsRejectReason::None;
  result.accepted = true;
  return result;
}

inline constexpr ParallelPassEconomicsAccounting
accountParallelPassEconomics(
    const ParallelPassEconomicsDecision& decision) noexcept {
  ParallelPassEconomicsAccounting result{};
  if (!decision.considered) {
    return result;
  }
  result.considered = 1u;
  if (decision.accepted &&
      decision.reject == ParallelPassEconomicsRejectReason::None) {
    result.accepted = 1u;
  } else {
    result.serialFallback = 1u;
    const auto reason =
        decision.reject == ParallelPassEconomicsRejectReason::None ||
            decision.reject == ParallelPassEconomicsRejectReason::Count
        ? ParallelPassEconomicsRejectReason::InvalidOrOverflow
        : decision.reject;
    result.rejectionCounts[static_cast<std::size_t>(reason)] = 1u;
  }
  return result;
}

template <typename Accepted, typename SerialFallback>
ParallelPassEconomicsDecision dispatchParallelPassEconomics(
    const ParallelPassEconomicsSummary& summary,
    Accepted&& accepted,
    SerialFallback&& serialFallback) {
  const auto decision = classifyParallelPassEconomics(summary);
  if (decision.accepted) {
    std::forward<Accepted>(accepted)();
  } else {
    std::forward<SerialFallback>(serialFallback)();
  }
  return decision;
}

template <typename Observer>
bool observeParallelPassEconomicsCountersIfEnabled(
    bool perfEnabled,
    const ParallelPassEconomicsSummary& summary,
    Observer&& observer) {
  if (!perfEnabled) {
    return false;
  }
  std::forward<Observer>(observer)(
      summary, classifyParallelPassEconomics(summary));
  return true;
}

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
  NoTwoChildWork,
  PlannerInvariant,
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

// Coordinator-owned value inputs captured after source preambles have
// resolved capture and initializer state, but before command replay can emit
// Metal work.  The resulting proof is call-local and contains no borrowed
// payload or runtime object.
struct ParallelPassCoordinatorProofSnapshotInput {
  std::uint64_t firstPassActionEpoch = 0;
  bool queryAbsent = false;
  bool updateTextureAbsent = false;
  bool captureInactive = false;
  bool initializerIndependent = false;
  bool orderedControlAbsent = false;
  bool sidecarObservationAbsent = false;
};

constexpr ParallelPassCoordinatorProof
makeParallelPassCoordinatorProofSnapshot(
    const ParallelPassCoordinatorProofSnapshotInput& input) noexcept {
  ParallelPassCoordinatorProof proof{
      .firstPassActionEpoch = input.firstPassActionEpoch,
  };
  if (input.queryAbsent) {
    proof.flags |= ParallelPassQueryAbsent;
  }
  if (input.updateTextureAbsent) {
    proof.flags |= ParallelPassUpdateTextureAbsent;
  }
  if (input.captureInactive) {
    proof.flags |= ParallelPassCaptureInactive;
  }
  if (input.initializerIndependent) {
    proof.flags |= ParallelPassInitializerIndependent;
  }
  if (input.orderedControlAbsent) {
    proof.flags |= ParallelPassOrderedControlAbsent;
  }
  if (input.sidecarObservationAbsent) {
    proof.flags |= ParallelPassSidecarObservationAbsent;
  }
  return proof;
}

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
  std::array<std::uint32_t,
             kParallelRenderPassChildCapacity> childReplayOrdinalBegins{};
  std::array<std::uint32_t,
             kParallelRenderPassChildCapacity> childReplayOrdinalCounts{};
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
  // Multi-command passes are divided only at DrawRun command boundaries.
  // A single large DrawRun instead retains the existing draw-subrange split.
  bool childrenCoverCompleteCommands = false;
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
  BindingPsoMissing,
  BindingStage2ArgumentTable,
  BindingResourceArray,
  BindingMixedAbi,
  BindingOverrideRebuild,
  Count,
};

inline constexpr ParallelPassFallbackReason
parallelPassFallbackForBindingReject(
    ParallelPassBindingRejectReason reject) noexcept {
  switch (reject) {
  case ParallelPassBindingRejectReason::None:
    return ParallelPassFallbackReason::None;
  case ParallelPassBindingRejectReason::MissingPso:
    return ParallelPassFallbackReason::BindingPsoMissing;
  case ParallelPassBindingRejectReason::Stage2ArgumentTable:
    return ParallelPassFallbackReason::BindingStage2ArgumentTable;
  case ParallelPassBindingRejectReason::ResourceArray:
    return ParallelPassFallbackReason::BindingResourceArray;
  case ParallelPassBindingRejectReason::MixedAbi:
    return ParallelPassFallbackReason::BindingMixedAbi;
  case ParallelPassBindingRejectReason::OverrideRebuild:
    return ParallelPassFallbackReason::BindingOverrideRebuild;
  case ParallelPassBindingRejectReason::Count:
    return ParallelPassFallbackReason::BindingPsoMissing;
  }
  return ParallelPassFallbackReason::BindingPsoMissing;
}

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
  case SealedParallelPassSnapshotFallback::NoTwoChildWork:
    return ParallelPassFallbackReason::TooFewChildren;
  case SealedParallelPassSnapshotFallback::PlannerInvariant:
    return ParallelPassFallbackReason::PlanNotValidated;
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
  std::span<const std::uint32_t> childReplayOrdinalBegins{};
  std::span<const std::uint32_t> childReplayOrdinalCounts{};
  std::uint64_t passActionEpoch = 0;
  bool childrenCoverCompleteCommands = false;
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
  const bool hasChildSpans = !input.childReplayOrdinalBegins.empty() ||
      !input.childReplayOrdinalCounts.empty();
  if (hasChildSpans &&
      (input.childReplayOrdinalBegins.size() != input.ranges.size() ||
       input.childReplayOrdinalCounts.size() != input.ranges.size())) {
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
    if (hasChildSpans &&
        (input.childReplayOrdinalCounts[i] == 0u ||
         input.childReplayOrdinalBegins[i] !=
             input.ranges[i].replayOrdinalBegin)) {
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
  ParallelPassBindingSnapshot binding{};
  std::uint32_t replayOrdinalBegin = 0;
  std::uint32_t replayOrdinalCount = 0;
  std::uint32_t childOrdinal = 0;
  std::uint32_t localShadowOrdinal = 0;
  bool coversCompleteCommands = false;
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
    const bool hasExplicitSpans =
        input.childReplayOrdinalBegins.size() == input.ranges.size() &&
        input.childReplayOrdinalCounts.size() == input.ranges.size();
    storage.children[i] = ParallelPassChildPlan{
        .range = input.ranges[i],
        .firstDraw = input.firstDrawSnapshots[i],
        .replayOrdinalBegin = hasExplicitSpans
            ? input.childReplayOrdinalBegins[i]
            : input.ranges[i].replayOrdinalBegin,
        .replayOrdinalCount = hasExplicitSpans
            ? input.childReplayOrdinalCounts[i]
            : input.ranges[i].replayOrdinalCount,
        .childOrdinal = static_cast<std::uint32_t>(i),
        .localShadowOrdinal = static_cast<std::uint32_t>(i + 1u),
        .coversCompleteCommands = input.childrenCoverCompleteCommands,
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
  std::uint32_t previousReplayCount = 0u;
  std::uint32_t previousDrawEnd = 0u;
  std::uint32_t previousCommandIndex = 0u;
  for (std::size_t i = 0; i < children.size(); ++i) {
    const auto& child = children[i];
    const auto& range = child.range;
    if (range.kind != EncodePartitionRangeKind::DrawRunEntries ||
        range.replayOrdinalCount != 1u || range.drawEntryCount == 0u ||
        child.replayOrdinalCount == 0u ||
        child.replayOrdinalBegin >
            UINT32_MAX - child.replayOrdinalCount ||
        child.replayOrdinalBegin != range.replayOrdinalBegin ||
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
    if (child.binding.reject != ParallelPassBindingRejectReason::None) {
      return parallelPassFallbackForBindingReject(child.binding.reject);
    }
    if (!child.binding.complete || !child.binding.firstRenderPso.valid() ||
        child.binding.mode == ParallelPassDirectBindingMode::Count) {
      return ParallelPassFallbackReason::BindingPsoMissing;
    }
    if (i != 0u && child.binding.mode != children.front().binding.mode) {
      return ParallelPassFallbackReason::BindingMixedAbi;
    }
    if (i != 0u) {
      if (child.coversCompleteCommands !=
          children.front().coversCompleteCommands) {
        return ParallelPassFallbackReason::ChildRangeOrderInvalid;
      }
      if (child.coversCompleteCommands) {
        if (child.replayOrdinalBegin !=
            previousReplayOrdinal + previousReplayCount) {
          return ParallelPassFallbackReason::ChildRangeOrderInvalid;
        }
      } else if (range.replayOrdinalBegin < previousReplayOrdinal) {
        return ParallelPassFallbackReason::ChildRangeOrderInvalid;
      } else if (range.replayOrdinalBegin == previousReplayOrdinal) {
        if (range.entry.commandIndex != previousCommandIndex ||
            range.entry.drawParamIndex != previousDrawEnd) {
          return ParallelPassFallbackReason::ChildRangeOrderInvalid;
        }
      } else if (range.replayOrdinalBegin != previousReplayOrdinal + 1u ||
                 range.entry.commandIndex == previousCommandIndex) {
        return ParallelPassFallbackReason::ChildRangeOrderInvalid;
      }
    }
    previousReplayOrdinal = child.replayOrdinalBegin;
    previousReplayCount = child.replayOrdinalCount;
    previousCommandIndex = range.entry.commandIndex;
    previousDrawEnd = range.entry.drawParamIndex + range.drawEntryCount;
  }
  return ParallelPassFallbackReason::None;
}

// Backend is a deliberately small seam shared by deterministic native fakes
// and the production WMT parent/child implementation. Child creation is
// ordered and pre-effect. beginPassActions() is the effect edge;
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
  if constexpr (requires { backend.emitChildren(children); }) {
    const std::uint32_t failedChild = backend.emitChildren(children);
    if (failedChild != kParallelRenderPassNoFailedChild) {
      return failStop(ParallelPassFailurePhase::ChildEmission, failedChild);
    }
  } else {
    for (const auto& child : children) {
      if (!backend.emitChild(child)) {
        return failStop(ParallelPassFailurePhase::ChildEmission,
                        child.childOrdinal);
      }
    }
  }
  for (const auto& child : children) {
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
static_assert(std::is_trivially_copyable_v<ParallelPassBindingSnapshot>);
static_assert(std::is_standard_layout_v<ParallelPassBindingSnapshot>);
static_assert(std::is_trivially_copyable_v<ParallelPassEconomicsSummary>);
static_assert(std::is_standard_layout_v<ParallelPassEconomicsSummary>);
static_assert(
    std::is_trivially_copyable_v<ParallelPassWholeCommandSubdivision>);
static_assert(std::is_standard_layout_v<ParallelPassWholeCommandSubdivision>);
static_assert(
    std::is_trivially_copyable_v<ParallelPassCoordinatorProofSnapshotInput>);
static_assert(std::is_trivially_copyable_v<ParallelPassCommandLocator>);
static_assert(std::is_trivially_copyable_v<SealedParallelPassSnapshot>);
static_assert(std::is_standard_layout_v<SealedParallelPassSnapshot>);
static_assert(std::is_trivially_copyable_v<SealedParallelPassSnapshotBatch>);
static_assert(std::is_standard_layout_v<ParallelPassChildPlan>);
static_assert(std::is_trivially_copyable_v<ParallelPassPlanStorage>);

}  // namespace dxmt9::encoders
