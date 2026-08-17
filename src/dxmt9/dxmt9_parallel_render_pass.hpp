#pragma once

#include "dxmt9_encode_partition.hpp"
#include "dxmt9_source_semantics.hpp"
#include "dxmt9_uniform_dirty.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <tuple>
#include <type_traits>
#include <utility>

namespace dxmt9::encoders {

inline constexpr std::size_t kParallelRenderPassChildCapacity = 16u;
inline constexpr std::size_t kParallelRenderPassCandidateCapacity = 16u;

// ---------------------------------------------------------------------
// DXMT9_PARALLEL_PASS_DRAW_QUANTUM — diagnostic/tuning env knob.
// ---------------------------------------------------------------------
//
// The parallel sealed-pass builder (subdivideParallelPassDraws,
// subdivideParallelPassWholeCommands) and the economics classifier
// (classifyParallelPassEconomics) share `kProductionPartitionDrawThreshold`
// (dxmt9_encode_partition.hpp) as their two-child floor / eligibility
// quantum. This knob lets an experiment override that quantum for the
// PARALLEL path only, without touching the serial partition planner
// (planProductionEncodePartitions), which keeps using
// kProductionPartitionDrawThreshold directly and never reads this env var or
// its resolvers. It is a tuning/experiment surface, not a stable provider
// contract: see agents/rules/environment_variables_encoder.rules.md.
inline constexpr std::uint32_t kParallelPassDrawQuantumMin = 4u;
inline constexpr std::uint32_t kParallelPassDrawQuantumMax = 1024u;

// Pure parse+clamp of DXMT9_PARALLEL_PASS_DRAW_QUANTUM. A null, empty, "0",
// or unparseable string resolves to kProductionPartitionDrawThreshold
// (byte-identical default behavior) with `clamped=false`. A parsed value is
// clamped to [kParallelPassDrawQuantumMin, kParallelPassDrawQuantumMax];
// `clamped` reports whether clamping changed the parsed value so the
// env-reading wrapper can log exactly one bounded warning.
struct ParallelPassDrawQuantumResolution {
  std::uint32_t quantum = kProductionPartitionDrawThreshold;
  bool clamped = false;

  friend constexpr bool operator==(const ParallelPassDrawQuantumResolution&,
                                   const ParallelPassDrawQuantumResolution&) =
      default;
};

ParallelPassDrawQuantumResolution resolveParallelPassDrawQuantum(
    const char* env) noexcept;

// Process-once env reader; reads DXMT9_PARALLEL_PASS_DRAW_QUANTUM via
// std::getenv on first call, caches the clamped quantum, and logs one
// bounded warning if the parsed value needed clamping.
std::uint32_t resolveParallelPassDrawQuantumFromEnv();
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

  friend constexpr bool operator==(const ParallelPassChildSubdivision&,
                                   const ParallelPassChildSubdivision&) =
      default;
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
subdivideParallelPassWholeCommands(
    std::uint32_t replayOrdinalBegin,
    std::uint32_t commandCount,
    std::uint64_t totalDraws,
    DrawCountAt&& drawCountAt,
    std::uint32_t drawQuantum = kProductionPartitionDrawThreshold) noexcept {
  ParallelPassWholeCommandSubdivision result{};
  if (drawQuantum == 0u || commandCount < 2u ||
      totalDraws < 2ull * drawQuantum) {
    result.failure = ParallelPassWholeCommandPlanFailure::NoTwoChildWork;
    return result;
  }
  const std::uint32_t maximumChildren = static_cast<std::uint32_t>(
      std::min<std::uint64_t>({
          kParallelRenderPassChildCapacity, commandCount,
          totalDraws / drawQuantum}));
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
        groupDraws >= drawQuantum &&
        remainingDraws >= drawQuantum &&
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
      groupDraws < drawQuantum ||
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
subdivideParallelPassDraws(
    std::uint64_t totalDraws,
    std::uint32_t drawQuantum = kProductionPartitionDrawThreshold) noexcept {
  ParallelPassChildSubdivision result{};
  if (drawQuantum == 0u || totalDraws > UINT32_MAX) {
    return result;
  }
  const std::uint64_t boundedChildren = std::min<std::uint64_t>(
      kParallelRenderPassChildCapacity, totalDraws / drawQuantum);
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
    if (count < drawQuantum ||
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
    const ParallelPassEconomicsSummary& summary,
    std::uint32_t drawQuantum = kProductionPartitionDrawThreshold) noexcept {
  ParallelPassEconomicsDecision result{.considered = true};
  if (drawQuantum == 0u) {
    result.reject = ParallelPassEconomicsRejectReason::InvalidOrOverflow;
    return result;
  }
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
  if (summary.minimumChildDraws < drawQuantum) {
    return reject(ParallelPassEconomicsRejectReason::ThinChild);
  }
  if (summary.maximumChildDraws - summary.minimumChildDraws > drawQuantum) {
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
    SerialFallback&& serialFallback,
    std::uint32_t drawQuantum = kProductionPartitionDrawThreshold) {
  const auto decision = classifyParallelPassEconomics(summary, drawQuantum);
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
    Observer&& observer,
    std::uint32_t drawQuantum = kProductionPartitionDrawThreshold) {
  if (!perfEnabled) {
    return false;
  }
  std::forward<Observer>(observer)(
      summary, classifyParallelPassEconomics(summary, drawQuantum));
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

// Total classification of every source command a sealed-pass observation can
// meet in the final effective replay order. `CoordinatorBoundary` names the
// non-child helper commands that own a short-lived Metal encoder of their own:
// the coordinator must have ended the render encoder before replaying them, so
// they terminate a pass interval at exactly the position `Clear`/`Present` do.
// A kind that is not enumerated here is `Unsupported` and fails the whole
// source closed, which keeps the batch-wide rejection path reachable for any
// command kind added later.
enum class ParallelPassCommandRole : std::uint8_t {
  Draw,
  ClearBoundary,
  PresentBoundary,
  CoordinatorBoundary,
  Unsupported,
};

inline constexpr ParallelPassCommandRole classifyParallelPassCommandRole(
    core::MetalCommandKind kind) noexcept {
  switch (kind) {
  case core::MetalCommandKind::DrawRun:
    return ParallelPassCommandRole::Draw;
  case core::MetalCommandKind::Clear:
    return ParallelPassCommandRole::ClearBoundary;
  case core::MetalCommandKind::Present:
    return ParallelPassCommandRole::PresentBoundary;
  case core::MetalCommandKind::SurfaceCopy:
  case core::MetalCommandKind::StretchRect:
  case core::MetalCommandKind::Readback:
  case core::MetalCommandKind::ColorFill:
  case core::MetalCommandKind::DepthResolve:
    return ParallelPassCommandRole::CoordinatorBoundary;
  }
  return ParallelPassCommandRole::Unsupported;
}

// A sealed pass may end only at a coordinator-owned command that the
// coordinator still replays serially at `replayOrdinalEnd`. Every accepted kind
// therefore stays outside the child ranges and inside the coverage proof
// (`R-BACK-2.70`).
inline constexpr bool parallelPassSealingKindAccepted(
    core::MetalCommandKind kind) noexcept {
  switch (classifyParallelPassCommandRole(kind)) {
  case ParallelPassCommandRole::ClearBoundary:
  case ParallelPassCommandRole::PresentBoundary:
  case ParallelPassCommandRole::CoordinatorBoundary:
    return true;
  case ParallelPassCommandRole::Draw:
  case ParallelPassCommandRole::Unsupported:
    return false;
  }
  return false;
}

// The attachment identity a `Clear` publishes. It is shared by the producer
// and by the certificate's independent epoch re-derivation so a leading-clear
// match cannot be decided by two different rules (`R-BACK-2.69`).
inline core::RenderAttachmentKey parallelPassAttachmentKeyForClear(
    const core::ClearCommandView& clear) noexcept {
  core::RenderAttachmentKey key{
      .color = clear.colorAttachments,
      .depthStencil = clear.depthStencil,
  };
  for (const auto& attachment : key.color) {
    key.sampleCount = std::max(key.sampleCount, attachment.sampleCount);
  }
  key.sampleCount = std::max(key.sampleCount, key.depthStencil.sampleCount);
  return key;
}

// The single implementation of the pass-action-epoch state machine. It is a
// pure fold over the effective replay order: no clock, no float, no
// allocation, and no dependence on iteration order beyond the ordinal
// sequence it is driven with. The producer drives it while sealing passes and
// the certificate drives it again over freshly read stream facts, so the two
// can never disagree about what the rule *is* — only about what the stream
// *was*, which is exactly the drift the certificate must catch.
struct ParallelPassActionEpochState {
  std::uint64_t epoch = 0;
  core::RenderAttachmentKey pendingClearAttachments{};
  core::RenderAttachmentKey activeAttachments{};
  bool pendingClear = false;
  bool active = false;
  bool valid = true;

  constexpr void advance() noexcept {
    if (!valid) {
      return;
    }
    if (epoch == std::numeric_limits<std::uint64_t>::max()) {
      valid = false;
      return;
    }
    ++epoch;
  }

  constexpr bool leadingClearMatches(
      const core::RenderAttachmentKey& attachments) const noexcept {
    return pendingClear && pendingClearAttachments == attachments;
  }

  // A `Clear` ends any open pass. A full-attachment clear with no rects can be
  // adopted as the next pass's leading clear; a scissored clear cannot, so it
  // consumes an epoch of its own.
  constexpr void onClearBoundary(
      bool rectsEmpty,
      const core::RenderAttachmentKey& clearAttachments) noexcept {
    const bool endedPass = active;
    active = false;
    if (endedPass || pendingClear) {
      advance();
    }
    if (rectsEmpty) {
      pendingClear = true;
      pendingClearAttachments = clearAttachments;
      return;
    }
    pendingClear = false;
    advance();
  }

  // `Present` and every non-child coordinator helper own a Metal encoder of
  // their own, so they terminate a pass interval at exactly the position
  // `Clear` occupies.
  constexpr void onCoordinatorBoundary() noexcept {
    active = false;
    pendingClear = false;
    advance();
  }

  // Returns true when this DrawRun opens a new pass interval; `epoch` is then
  // that pass's own action epoch.
  constexpr bool onDrawRun(
      const core::RenderAttachmentKey& attachments) noexcept {
    if (active && attachments != activeAttachments) {
      active = false;
      advance();
    }
    if (active) {
      return false;
    }
    if (pendingClear && !leadingClearMatches(attachments)) {
      advance();
    }
    pendingClear = false;
    active = true;
    activeAttachments = attachments;
    return true;
  }
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

  friend constexpr bool operator==(const ParallelPassCoordinatorProof&,
                                   const ParallelPassCoordinatorProof&) =
      default;

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
  // DXMT9_PARALLEL_PASS_DRAW_QUANTUM — see the resolver doc-comment near the
  // top of this file. Defaults to kProductionPartitionDrawThreshold so every
  // caller that does not set it explicitly (including every existing test)
  // keeps byte-identical behavior.
  std::uint32_t drawQuantum = kProductionPartitionDrawThreshold;
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
  std::array<std::uint64_t, kParallelRenderPassChildCapacity> childDrawCounts{};
  core::RenderAttachmentKey attachments{};
  core::ExactResourceSet attachmentWrites{};
  core::ExactResourceSet resourceReads{};
  core::CpuReadyTape::SourceRef source{};
  ParallelPassCommandLocator firstDraw{};
  ParallelPassCommandLocator leadingClear{};
  ParallelPassCommandLocator sealingCommand{};
  std::uint64_t seqId = 0;
  std::uint64_t passActionEpoch = 0;
  ParallelPassCoordinatorProof coordinatorProof{};
  std::uint64_t drawCount = 0;
  std::uint32_t replayOrdinalBegin = 0;
  std::uint32_t replayOrdinalEnd = 0;
  std::uint32_t childCount = 0;
  // Multi-command passes are divided only at DrawRun command boundaries.
  // A single large DrawRun instead retains the existing draw-subrange split.
  bool childrenCoverCompleteCommands = false;
  bool sealedAtSourceEnd = false;

  friend constexpr bool operator==(const SealedParallelPassSnapshot&,
                                   const SealedParallelPassSnapshot&) =
      default;

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

// Owner-issued authority for one synchronous snapshot lookup. The request is
// deliberately reduced to source identity and replay interval: callers
// cannot hand the authority a candidate sealing command, range vector, or
// child-count aggregate to echo back. The owner must resolve the complete
// sealed interval from its current source table and return that value-owned
// snapshot before validation continues; no authority state is retained.
// The epoch-relevant classification of exactly one replay ordinal. It carries
// no payload, span, or Metal object: the certificate re-derives an epoch from
// values only.
struct ParallelPassActionEpochFact {
  core::RenderAttachmentKey attachments{};
  core::MetalCommandKind kind = core::MetalCommandKind::DrawRun;
  bool clearRectsEmpty = false;
};

// Reads the epoch-relevant facts of exactly one replay ordinal from a
// generation-pinned effective replay stream. Shared by the production witness
// and by native fixtures so both fold identical inputs.
inline bool readParallelPassActionEpochFact(
    const EncodePartitionReplayStream& stream, std::uint32_t replayOrdinal,
    ParallelPassActionEpochFact& fact) noexcept {
  if (!stream.valid || replayOrdinal >= stream.replayOrdinalCount()) {
    return false;
  }
  std::uint32_t commandIndex = 0u;
  if (!stream.commandIndexAt(static_cast<std::size_t>(replayOrdinal),
                             commandIndex)) {
    return false;
  }
  const auto command = stream.source.payload.commandAt(commandIndex);
  fact = {};
  fact.kind = command.kind();
  switch (classifyParallelPassCommandRole(fact.kind)) {
  case ParallelPassCommandRole::ClearBoundary:
    if (!command.clear.has_value()) {
      return false;
    }
    fact.attachments = parallelPassAttachmentKeyForClear(*command.clear);
    fact.clearRectsEmpty = command.clear->rects.empty();
    return true;
  case ParallelPassCommandRole::Draw:
    if (!command.command.drawState.hot) {
      return false;
    }
    fact.attachments =
        core::makeRenderAttachmentKey(*command.command.drawState.hot);
    return true;
  case ParallelPassCommandRole::PresentBoundary:
  case ParallelPassCommandRole::CoordinatorBoundary:
    return true;
  case ParallelPassCommandRole::Unsupported:
    return false;
  }
  return false;
}

// Coordinator-issued reader over the generation-pinned effective replay
// stream. `seedEpoch` is the coordinator's own source-wide starting epoch —
// the same value it published in the source-wide coordinator proof — so the
// certificate never takes the seed from the snapshot it is checking.
// `replayOrdinalCount` bounds the fold; the reader is borrowed only for the
// synchronous validation call.
struct ParallelPassActionEpochWitness {
  const void* context = nullptr;
  bool (*read)(const void* context,
               const core::CpuReadyTape::SourceRef& source,
               std::uint64_t seqId, std::uint32_t replayOrdinal,
               ParallelPassActionEpochFact& fact) noexcept = nullptr;
  std::uint64_t seedEpoch = 0;
  std::uint32_t replayOrdinalCount = 0;

  constexpr bool valid() const noexcept {
    return context != nullptr && read != nullptr && seedEpoch != 0u &&
        replayOrdinalCount != 0u;
  }
};

struct ParallelPassActionEpochDerivation {
  std::uint64_t epoch = 0;
  bool valid = false;

  friend constexpr bool operator==(const ParallelPassActionEpochDerivation&,
                                   const ParallelPassActionEpochDerivation&) =
      default;
};

// Independent re-derivation of one pass's action epoch. It walks the
// generation-pinned effective replay stream from ordinal zero in replay order,
// classifies every command through the shared classifier, and folds the shared
// epoch state machine. The result is accepted only when `replayOrdinalBegin`
// is itself the DrawRun that opens a pass, which is what binds the derived
// number to *this* interval rather than to some other pass of the same source.
// The fold is pure and deterministic: repeating it over the same stream
// produces the same value.
inline ParallelPassActionEpochDerivation deriveParallelPassActionEpoch(
    const core::CpuReadyTape::SourceRef& source, std::uint64_t seqId,
    std::uint32_t replayOrdinalBegin,
    const ParallelPassActionEpochWitness& witness) noexcept {
  ParallelPassActionEpochDerivation result{};
  if (!witness.valid() || !source.valid() || seqId == 0u ||
      replayOrdinalBegin >= witness.replayOrdinalCount) {
    return result;
  }
  ParallelPassActionEpochState state{.epoch = witness.seedEpoch};
  for (std::uint32_t ordinal = 0u; ordinal <= replayOrdinalBegin; ++ordinal) {
    ParallelPassActionEpochFact fact{};
    if (!witness.read(witness.context, source, seqId, ordinal, fact)) {
      return {};
    }
    const bool target = ordinal == replayOrdinalBegin;
    switch (classifyParallelPassCommandRole(fact.kind)) {
    case ParallelPassCommandRole::Unsupported:
      return {};
    case ParallelPassCommandRole::ClearBoundary:
      if (target) {
        return {};
      }
      state.onClearBoundary(fact.clearRectsEmpty, fact.attachments);
      break;
    case ParallelPassCommandRole::PresentBoundary:
    case ParallelPassCommandRole::CoordinatorBoundary:
      if (target) {
        return {};
      }
      state.onCoordinatorBoundary();
      break;
    case ParallelPassCommandRole::Draw: {
      const bool started = state.onDrawRun(fact.attachments);
      if (!target) {
        break;
      }
      if (!started || !state.valid || state.epoch == 0u) {
        return {};
      }
      result.epoch = state.epoch;
      result.valid = true;
      return result;
    }
    }
    if (!state.valid) {
      return {};
    }
  }
  return {};
}

struct ParallelPassSnapshotAuthority {
  const void* context = nullptr;
  bool (*resolve)(const void* context,
                  const core::CpuReadyTape::SourceRef& source,
                  std::uint64_t seqId,
                  std::uint32_t replayOrdinalBegin,
                  std::uint32_t replayOrdinalEnd,
                  SealedParallelPassSnapshot& authoritative) noexcept =
      nullptr;

  constexpr bool valid() const noexcept {
    return context != nullptr && resolve != nullptr;
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

// Draw-size buckets for a sealed pass candidate's total draw count, sampled
// at the same site that increments `sealedCount` (boundaryComplete, before
// the accept/reject split). Always-on: one comparison chain plus one array
// slot per sealed pass. Bucket sum must equal `sealedCount`
// (parallel_render_pass_spec.cpp pins the conservation).
enum class SealedPassDrawBucket : std::uint8_t {
  Under8 = 0,
  From8To15,
  From16To31,
  From32To63,
  From64To127,
  From128To255,
  From256Plus,
  Count,
};

inline constexpr SealedPassDrawBucket classifySealedPassDrawBucket(
    std::uint64_t drawCount) noexcept {
  if (drawCount < 8u) return SealedPassDrawBucket::Under8;
  if (drawCount < 16u) return SealedPassDrawBucket::From8To15;
  if (drawCount < 32u) return SealedPassDrawBucket::From16To31;
  if (drawCount < 64u) return SealedPassDrawBucket::From32To63;
  if (drawCount < 128u) return SealedPassDrawBucket::From64To127;
  if (drawCount < 256u) return SealedPassDrawBucket::From128To255;
  return SealedPassDrawBucket::From256Plus;
}

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
  // Non-child coordinator commands met in this source. Every one of them stays
  // at its serial position; `coordinatorSplits` is the subset that also failed
  // a pass interval closed because the same attachment resumed immediately
  // afterwards, so one logical pass would otherwise have been split.
  std::uint32_t coordinatorBoundaries = 0;
  std::uint32_t coordinatorSplits = 0;
  // Indexed by SealedPassDrawBucket; sum equals sealedCount.
  std::array<std::uint32_t,
             static_cast<std::size_t>(SealedPassDrawBucket::Count)>
      sealedDrawBuckets{};
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

enum class ParallelPassSemanticPlanFailure : std::uint8_t {
  None,
  MissingSnapshot,
  SourceIdentity,
  PassIdentity,
  CoordinatorProof,
  AttachmentProof,
  ResourceProof,
  FirstDrawProof,
  ChildCapacity,
  ChildPlan,
  Coverage,
  Arithmetic,
  Count,
};

// This is a value-owned certificate. It intentionally copies the bounded
// snapshot and child plans so retaining the result cannot retain a payload,
// page, callback, or other borrowed storage.
struct ParallelPassSemanticPlanValidation;
struct ParallelPassCoverageResolver;

class ParallelPassSemanticPlanView {
 public:
  constexpr bool valid() const noexcept {
    return childCount_ >= 2u &&
        childCount_ <= kParallelRenderPassChildCapacity;
  }
  constexpr const SealedParallelPassSnapshot& snapshot() const noexcept {
    return snapshot_;
  }
  constexpr std::span<const ParallelPassChildPlan> children() const noexcept {
    return std::span<const ParallelPassChildPlan>(children_.data(), childCount_);
  }

 private:
  friend struct ParallelPassSemanticPlanValidation;
  friend ParallelPassSemanticPlanValidation validateParallelPassSemanticPlan(
      const SealedParallelPassSnapshot*,
      std::span<const ParallelPassChildPlan>,
      const ParallelPassSnapshotAuthority&,
      const ParallelPassCoverageResolver&,
      const ParallelPassActionEpochWitness&) noexcept;
  SealedParallelPassSnapshot snapshot_{};
  std::array<ParallelPassChildPlan, kParallelRenderPassChildCapacity>
      children_{};
  std::uint32_t childCount_ = 0u;
};

struct ParallelPassSemanticPlanValidation {
  ParallelPassSemanticPlanFailure failure =
      ParallelPassSemanticPlanFailure::MissingSnapshot;
  ParallelPassSemanticPlanView plan{};

  constexpr bool accepted() const noexcept {
    return failure == ParallelPassSemanticPlanFailure::None && plan.valid();
  }
};

enum class ParallelPassCoverageFoldFailure : std::uint8_t {
  None,
  NotStarted,
  CommandOrder,
  CommandOverlap,
  EmptyCommand,
  CommandArithmetic,
  CommandContiguity,
  DrawArithmetic,
  SubrangeCapacity,
  Count,
};

// Streaming exact coverage accumulator (`R-BACK-2.70`). It replaces a fixed
// row array with O(1) state, so a child owning more commands than any array
// capacity is a normal input rather than a rejection.
//
// Every accumulator is exact. There is deliberately no hash summary anywhere
// in this type: a hash can collide, and a colliding summary would admit a
// false accept — the one failure mode a coverage proof must not have. The
// per-row predicates are enforced at append time against the previous row's
// boundary, which is what makes order, contiguity, and non-overlap decidable
// without retaining the rows. Only the first row survives the walk, because
// it is the only one the post-walk predicates read.
//
// One predicate is strictly stronger than the array form it replaces. The
// stored-row version detected a repeated `commandIndex` with an O(n) scan
// over prior rows; the fold requires whole-command rows to carry strictly
// increasing `commandIndex`, which implies duplicate-freedom and also rejects
// an out-of-order row set the scan accepted. That direction is fail-closed:
// it can only shrink the accepted set.
class ParallelPassCoverageFold {
 public:
  struct Command {
    std::uint32_t replayOrdinal = 0u;
    std::uint32_t commandIndex = 0u;
    std::uint32_t drawParamBegin = 0u;
    std::uint32_t drawParamCount = 0u;

    friend constexpr bool operator==(const Command&, const Command&) =
        default;
  };

  constexpr void open(std::uint32_t replayOrdinalBegin,
                      bool wholeCommands) noexcept {
    *this = {};
    replayOrdinalBegin_ = replayOrdinalBegin;
    wholeCommands_ = wholeCommands;
    opened_ = true;
  }

  constexpr bool append(const Command& command) noexcept {
    if (!opened_ ||
        (failure_ != ParallelPassCoverageFoldFailure::None &&
         failure_ != ParallelPassCoverageFoldFailure::NotStarted)) {
      return recordFailure(ParallelPassCoverageFoldFailure::NotStarted);
    }
    if (commandCount_ == std::numeric_limits<std::uint32_t>::max()) {
      return recordFailure(ParallelPassCoverageFoldFailure::CommandArithmetic);
    }
    if (!wholeCommands_ && commandCount_ != 0u) {
      return recordFailure(ParallelPassCoverageFoldFailure::SubrangeCapacity);
    }
    if (static_cast<std::uint64_t>(replayOrdinalBegin_) + commandCount_ !=
        static_cast<std::uint64_t>(command.replayOrdinal)) {
      return recordFailure(ParallelPassCoverageFoldFailure::CommandOrder);
    }
    if (command.drawParamCount == 0u) {
      return recordFailure(ParallelPassCoverageFoldFailure::EmptyCommand);
    }
    if (command.drawParamBegin >
        std::numeric_limits<std::uint32_t>::max() - command.drawParamCount) {
      return recordFailure(ParallelPassCoverageFoldFailure::CommandArithmetic);
    }
    if (commandCount_ != 0u &&
        command.commandIndex <= previousCommandIndex_) {
      return recordFailure(ParallelPassCoverageFoldFailure::CommandOverlap);
    }
    if (wholeCommands_ && commandCount_ != 0u &&
        command.drawParamBegin != previousDrawParamEnd_) {
      return recordFailure(ParallelPassCoverageFoldFailure::CommandContiguity);
    }
    if (drawTotal_ >
        std::numeric_limits<std::uint64_t>::max() - command.drawParamCount) {
      return recordFailure(ParallelPassCoverageFoldFailure::DrawArithmetic);
    }
    if (commandCount_ == 0u) {
      first_ = command;
    }
    previousCommandIndex_ = command.commandIndex;
    previousDrawParamEnd_ = command.drawParamBegin + command.drawParamCount;
    drawTotal_ += command.drawParamCount;
    ++commandCount_;
    failure_ = ParallelPassCoverageFoldFailure::None;
    return true;
  }

  constexpr bool valid() const noexcept {
    return opened_ && commandCount_ != 0u &&
        failure_ == ParallelPassCoverageFoldFailure::None;
  }
  constexpr const Command& first() const noexcept { return first_; }
  constexpr std::uint64_t drawTotal() const noexcept { return drawTotal_; }
  constexpr std::uint32_t commandCount() const noexcept {
    return commandCount_;
  }
  constexpr std::uint32_t replayOrdinalBegin() const noexcept {
    return replayOrdinalBegin_;
  }
  constexpr bool wholeCommands() const noexcept { return wholeCommands_; }
  // First failure only: a later append cannot overwrite the locator that
  // explains why the child was rejected.
  constexpr ParallelPassCoverageFoldFailure failure() const noexcept {
    return failure_;
  }
  constexpr std::uint32_t failureCommand() const noexcept {
    return failureCommand_;
  }

  friend constexpr bool operator==(const ParallelPassCoverageFold&,
                                   const ParallelPassCoverageFold&) = default;

 private:
  constexpr bool recordFailure(
      ParallelPassCoverageFoldFailure reason) noexcept {
    if (failure_ == ParallelPassCoverageFoldFailure::None ||
        failure_ == ParallelPassCoverageFoldFailure::NotStarted) {
      failure_ = reason;
      failureCommand_ = commandCount_;
    }
    return false;
  }

  Command first_{};
  std::uint64_t drawTotal_ = 0u;
  std::uint32_t commandCount_ = 0u;
  std::uint32_t previousCommandIndex_ = 0u;
  std::uint32_t previousDrawParamEnd_ = 0u;
  std::uint32_t replayOrdinalBegin_ = 0u;
  std::uint32_t failureCommand_ = UINT32_MAX;
  ParallelPassCoverageFoldFailure failure_ =
      ParallelPassCoverageFoldFailure::NotStarted;
  bool wholeCommands_ = false;
  bool opened_ = false;
};

struct ParallelPassResolvedCoverage {
  std::uint64_t drawCount = 0u;
  using Command = ParallelPassCoverageFold::Command;
  ParallelPassCoverageFold commands{};
  core::ExactResourceSet reads{};
  core::ExactResourceSet writes{};
  core::RenderAttachmentKey attachments{};
  core::RenderRoute route = core::RenderRoute::Unknown;
  std::uint64_t passActionEpoch = 0u;
};

// Exact coverage is owned by the current source resolver. The callback is
// borrowed only for the synchronous validation call and re-resolves every
// replay ordinal and DrawParam range, returning the exact child-wide facts.
// No callback state is retained in the certificate or selector result.
struct ParallelPassCoverageResolver {
  const void* context = nullptr;
  bool (*resolve)(const void* context,
                  const SealedParallelPassSnapshot& snapshot,
                  const ParallelPassChildPlan& child,
                  ParallelPassResolvedCoverage& coverage) noexcept = nullptr;

  constexpr bool valid() const noexcept {
    return context != nullptr && resolve != nullptr;
  }
};

inline bool parallelPassExactResourceSetValid(
    const core::ExactResourceSet& resources) noexcept {
  if (!resources.complete() || !resources.canonicalized() ||
      resources.count > resources.handles.size()) {
    return false;
  }
  for (std::uint32_t i = 0u; i < resources.count; ++i) {
    if (resources.handles[i] == 0u) {
      return false;
    }
    for (std::uint32_t j = i + 1u; j < resources.count; ++j) {
      if (resources.handles[i] == resources.handles[j]) {
        return false;
      }
    }
  }
  return true;
}

inline bool parallelPassExactResourceSetEqual(
    const core::ExactResourceSet& left,
    const core::ExactResourceSet& right) noexcept {
  if (!parallelPassExactResourceSetValid(left) ||
      !parallelPassExactResourceSetValid(right) ||
      left.count != right.count) {
    return false;
  }
  for (std::uint32_t i = 0u; i < left.count; ++i) {
    bool found = false;
    for (std::uint32_t j = 0u; j < right.count; ++j) {
      found |= left.handles[i] == right.handles[j];
    }
    if (!found) {
      return false;
    }
  }
  return true;
}

inline bool parallelPassExactResourceSetContains(
    const core::ExactResourceSet& container,
    const core::ExactResourceSet& contained) noexcept {
  if (!parallelPassExactResourceSetValid(container) ||
      !parallelPassExactResourceSetValid(contained)) {
    return false;
  }
  for (std::uint32_t i = 0u; i < contained.count; ++i) {
    bool found = false;
    for (std::uint32_t j = 0u; j < container.count; ++j) {
      found |= contained.handles[i] == container.handles[j];
    }
    if (!found) {
      return false;
    }
  }
  return true;
}

inline bool validateParallelPassSemanticPlanCoverage(
    const SealedParallelPassSnapshot& snapshot,
    std::span<const ParallelPassChildPlan> children,
    std::span<const ParallelPassResolvedCoverage> resolvedCoverage) noexcept {
  if (snapshot.replayOrdinalBegin >= snapshot.replayOrdinalEnd ||
      snapshot.drawCount == 0u || children.size() != snapshot.childCount ||
      resolvedCoverage.size() != children.size() || children.empty()) {
    return false;
  }
  std::uint64_t coveredDraws = 0u;
  std::uint32_t previousReplayEnd = snapshot.replayOrdinalBegin;
  std::uint32_t previousCommand = 0u;
  std::uint32_t previousDrawEnd = 0u;
  for (std::size_t i = 0u; i < children.size(); ++i) {
    const auto& child = children[i];
    const auto& range = child.range;
    if (child.childOrdinal != i || child.replayOrdinalBegin !=
            snapshot.childReplayOrdinalBegins[i] ||
        child.replayOrdinalCount != snapshot.childReplayOrdinalCounts[i] ||
        snapshot.ranges[i] != range ||
        snapshot.firstDraws[i] != child.firstDraw ||
        range.entry.source.tapeSource != snapshot.source ||
        range.entry.source.seqId != snapshot.seqId ||
        range.replayOrdinalBegin != child.replayOrdinalBegin ||
        range.replayOrdinalCount == 0u || child.replayOrdinalCount == 0u ||
        child.replayOrdinalBegin >
            UINT32_MAX - child.replayOrdinalCount ||
        snapshot.childDrawCounts[i] == 0u) {
      return false;
    }
    if (i == 0u && child.replayOrdinalBegin != snapshot.replayOrdinalBegin) {
      return false;
    }
    if (snapshot.childrenCoverCompleteCommands) {
      if (child.replayOrdinalBegin != previousReplayEnd) {
        return false;
      }
      previousReplayEnd = child.replayOrdinalBegin + child.replayOrdinalCount;
    } else {
      if (child.replayOrdinalCount != 1u ||
          child.replayOrdinalBegin != snapshot.replayOrdinalBegin) {
        return false;
      }
      if (i != 0u &&
          (range.entry.commandIndex != previousCommand ||
           range.entry.drawParamIndex != previousDrawEnd)) {
        return false;
      }
      if (range.drawEntryCount == 0u ||
          range.entry.drawParamIndex >
              UINT32_MAX - range.drawEntryCount) {
        return false;
      }
      previousCommand = range.entry.commandIndex;
      previousDrawEnd = range.entry.drawParamIndex + range.drawEntryCount;
    }
    const auto& coverage = resolvedCoverage[i];
    const auto& fold = coverage.commands;
    if (coverage.drawCount == 0u || !fold.valid() ||
        fold.wholeCommands() != snapshot.childrenCoverCompleteCommands ||
        fold.replayOrdinalBegin() != child.replayOrdinalBegin ||
        fold.commandCount() > child.replayOrdinalCount ||
        coverage.drawCount != snapshot.childDrawCounts[i] ||
        coverage.attachments != snapshot.attachments ||
        coverage.route != child.firstDraw.entryRender.route ||
        coverage.passActionEpoch != snapshot.passActionEpoch ||
        coveredDraws > UINT64_MAX - coverage.drawCount) {
      return false;
    }
    if (snapshot.childrenCoverCompleteCommands
            ? fold.commandCount() != child.replayOrdinalCount
            : fold.commandCount() != 1u) {
      return false;
    }
    if (fold.first().commandIndex != range.entry.commandIndex ||
        (snapshot.childrenCoverCompleteCommands &&
         (fold.first().drawParamBegin != range.entry.drawParamIndex ||
          fold.first().drawParamCount != range.drawEntryCount))) {
      return false;
    }
    coveredDraws += coverage.drawCount;
  }
  return coveredDraws == snapshot.drawCount &&
      (snapshot.childrenCoverCompleteCommands
           ? previousReplayEnd == snapshot.replayOrdinalEnd
           : snapshot.replayOrdinalEnd == snapshot.replayOrdinalBegin + 1u);
}

inline ParallelPassSemanticPlanValidation validateParallelPassSemanticPlan(
    const SealedParallelPassSnapshot* snapshot,
    std::span<const ParallelPassChildPlan> children,
    const ParallelPassSnapshotAuthority& authority,
    const ParallelPassCoverageResolver& resolver,
    const ParallelPassActionEpochWitness& epochWitness) noexcept {
  ParallelPassSemanticPlanValidation result{};
  if (snapshot == nullptr) {
    return result;
  }
  SealedParallelPassSnapshot authoritative{};
  if (!authority.valid() ||
      !authority.resolve(authority.context, snapshot->source, snapshot->seqId,
                         snapshot->replayOrdinalBegin,
                         snapshot->replayOrdinalEnd, authoritative) ||
      authoritative != *snapshot) {
    result.failure = ParallelPassSemanticPlanFailure::SourceIdentity;
    return result;
  }
  snapshot = &authoritative;
  result.failure = ParallelPassSemanticPlanFailure::SourceIdentity;
  if (!snapshot->source.valid() || snapshot->seqId == 0u ||
      !snapshot->firstDraw.source.tapeSource.valid() ||
      snapshot->firstDraw.source.tapeSource != snapshot->source ||
      snapshot->firstDraw.source.seqId != snapshot->seqId ||
      !snapshot->firstDraw.valid || snapshot->firstDraw.kind !=
          core::MetalCommandKind::DrawRun) {
    return result;
  }
  result.failure = ParallelPassSemanticPlanFailure::PassIdentity;
  if (snapshot->passActionEpoch == 0u ||
      snapshot->replayOrdinalBegin >= snapshot->replayOrdinalEnd ||
      (!snapshot->sealedAtSourceEnd && !snapshot->sealingCommand.valid) ||
      (snapshot->sealedAtSourceEnd && snapshot->sealingCommand.valid) ||
      (snapshot->sealingCommand.valid &&
       (snapshot->sealingCommand.source.tapeSource != snapshot->source ||
        snapshot->sealingCommand.source.seqId != snapshot->seqId ||
        snapshot->sealingCommand.replayOrdinal !=
            snapshot->replayOrdinalEnd ||
        !parallelPassSealingKindAccepted(snapshot->sealingCommand.kind))) ||
      snapshot->coordinatorProof.firstPassActionEpoch !=
          snapshot->passActionEpoch) {
    return result;
  }
  // The stored stamp is not evidence of itself. The certificate re-derives the
  // epoch for this sealed interval from the generation-pinned stream and only
  // then accepts the producer's pass-local coordinator proof. Source and
  // storage generations have already failed closed above, so a stale source
  // can never reach this fold.
  const auto derivedEpoch = deriveParallelPassActionEpoch(
      snapshot->source, snapshot->seqId, snapshot->replayOrdinalBegin,
      epochWitness);
  if (!derivedEpoch.valid ||
      derivedEpoch.epoch != snapshot->passActionEpoch) {
    return result;
  }
  result.failure = ParallelPassSemanticPlanFailure::CoordinatorProof;
  if ((snapshot->coordinatorProof.flags &
       kParallelPassCoordinatorProofComplete) !=
          kParallelPassCoordinatorProofComplete ||
      snapshot->coordinatorProof.flags !=
          kParallelPassCoordinatorProofComplete) {
    return result;
  }
  result.failure = ParallelPassSemanticPlanFailure::AttachmentProof;
  if (snapshot->attachments.sampleCount == 0u) {
    return result;
  }
  result.failure = ParallelPassSemanticPlanFailure::ResourceProof;
  if (!parallelPassExactResourceSetValid(snapshot->attachmentWrites) ||
      !parallelPassExactResourceSetValid(snapshot->resourceReads)) {
    return result;
  }
  result.failure = ParallelPassSemanticPlanFailure::ChildCapacity;
  if (children.size() != snapshot->childCount || children.size() < 2u ||
      children.size() > kParallelRenderPassChildCapacity ||
      snapshot->childCount != snapshot->firstDrawView().size()) {
    return result;
  }
  if (snapshot->firstDraw.replayOrdinal != snapshot->replayOrdinalBegin ||
      snapshot->firstDraw.source.seqId != snapshot->seqId ||
      snapshot->firstDraw.source.tapeSource != snapshot->source ||
      snapshot->firstDraw.kind != core::MetalCommandKind::DrawRun ||
      !snapshot->firstDraw.valid ||
      children.front().range.entry.commandIndex !=
          snapshot->firstDraw.commandIndex ||
      children.front().range.entry.source != snapshot->firstDraw.source ||
      children.front().replayOrdinalBegin != snapshot->firstDraw.replayOrdinal ||
      children.front().firstDraw.provenance != children.front().range.entry) {
    result.failure = ParallelPassSemanticPlanFailure::FirstDrawProof;
    return result;
  }
  result.failure = ParallelPassSemanticPlanFailure::FirstDrawProof;
  core::ExactResourceSet expectedReads{};
  expectedReads.flags = core::ExactResourceSetComplete |
      core::ExactResourceSetCanonicalized;
  core::ExactResourceSet expectedWrites{};
  expectedWrites.flags = core::ExactResourceSetComplete |
      core::ExactResourceSetCanonicalized;
  std::array<ParallelPassResolvedCoverage,
             kParallelRenderPassChildCapacity>
      resolvedCoverage{};
  for (std::size_t i = 0u; i < children.size(); ++i) {
    const auto& firstDraw = snapshot->firstDraws[i];
    const auto& child = children[i];
    if (snapshot->ranges[i] != child.range ||
        child.childOrdinal != i ||
        child.replayOrdinalBegin != snapshot->childReplayOrdinalBegins[i] ||
        child.replayOrdinalCount != snapshot->childReplayOrdinalCounts[i] ||
        child.replayOrdinalCount == 0u ||
        child.replayOrdinalBegin >
            UINT32_MAX - child.replayOrdinalCount ||
        child.range.replayOrdinalBegin != child.replayOrdinalBegin ||
        firstDraw != child.firstDraw || !firstDraw.complete ||
        firstDraw.generation != snapshot->seqId ||
        firstDraw.generation == 0u ||
        child.range.entry.source.seqId != snapshot->seqId ||
        child.firstDraw.generation != snapshot->seqId ||
        snapshot->firstDraws[i].provenance != children[i].range.entry ||
        snapshot->firstDraws[i].entryRender.attachments !=
            snapshot->attachments ||
        snapshot->firstDraws[i].entryRender.passActionEpoch !=
            snapshot->passActionEpoch ||
        snapshot->firstDraws[i].entryRender.route == core::RenderRoute::Unknown ||
        !parallelPassExactResourceSetValid(
            snapshot->firstDraws[i].entryRender.entryReads)) {
      return result;
    }
    if (!resolver.valid() ||
        !resolver.resolve(resolver.context, *snapshot, child,
                          resolvedCoverage[i]) ||
        resolvedCoverage[i].drawCount == 0u ||
        resolvedCoverage[i].attachments != snapshot->attachments ||
        resolvedCoverage[i].route != firstDraw.entryRender.route ||
        resolvedCoverage[i].passActionEpoch != snapshot->passActionEpoch ||
        !parallelPassExactResourceSetValid(resolvedCoverage[i].reads) ||
        !parallelPassExactResourceSetValid(resolvedCoverage[i].writes) ||
        resolvedCoverage[i].reads.overlaps(resolvedCoverage[i].writes) ||
        !parallelPassExactResourceSetContains(
            resolvedCoverage[i].reads, firstDraw.entryRender.entryReads)) {
      return result;
    }
    const auto& fold = resolvedCoverage[i].commands;
    if (!fold.valid() ||
        fold.wholeCommands() != snapshot->childrenCoverCompleteCommands ||
        fold.replayOrdinalBegin() != child.replayOrdinalBegin ||
        fold.commandCount() > child.replayOrdinalCount ||
        fold.first().commandIndex != child.range.entry.commandIndex ||
        (snapshot->childrenCoverCompleteCommands
             ? fold.commandCount() != child.replayOrdinalCount
             : fold.commandCount() != 1u)) {
      return result;
    }
    if ((snapshot->childrenCoverCompleteCommands &&
         fold.drawTotal() != resolvedCoverage[i].drawCount) ||
        (!snapshot->childrenCoverCompleteCommands &&
         resolvedCoverage[i].drawCount != child.range.drawEntryCount)) {
      return result;
    }
    if (snapshot->childrenCoverCompleteCommands) {
      if (child.range.entry.drawParamIndex != fold.first().drawParamBegin ||
          fold.first().drawParamCount != child.range.drawEntryCount) {
        return result;
      }
    } else {
      const auto& resolved = fold.first();
      if (child.range.entry.drawParamIndex < resolved.drawParamBegin ||
          child.range.entry.drawParamIndex - resolved.drawParamBegin >
              resolved.drawParamCount ||
          child.range.drawEntryCount >
              resolved.drawParamCount -
                  (child.range.entry.drawParamIndex -
                   resolved.drawParamBegin)) {
        return result;
      }
    }
    for (std::uint32_t resource = 0u;
         resource < resolvedCoverage[i].reads.count;
         ++resource) {
      if (!expectedReads.add(resolvedCoverage[i].reads.handles[resource])) {
        return result;
      }
    }
    for (std::uint32_t resource = 0u;
         resource < resolvedCoverage[i].writes.count;
         ++resource) {
      if (!expectedWrites.add(resolvedCoverage[i].writes.handles[resource])) {
        return result;
      }
    }
  }
  if (!parallelPassExactResourceSetEqual(expectedReads,
                                         snapshot->resourceReads) ||
      !parallelPassExactResourceSetEqual(expectedWrites,
                                         snapshot->attachmentWrites) ||
      expectedReads.overlaps(expectedWrites) ||
      snapshot->resourceReads.overlaps(snapshot->attachmentWrites)) {
    return result;
  }
  if (!snapshot->childrenCoverCompleteCommands) {
    const auto& first = resolvedCoverage.front().commands.first();
    std::uint64_t expectedDrawParam = first.drawParamBegin;
    for (std::size_t i = 0u; i < children.size(); ++i) {
      const auto& child = children[i];
      const auto& command = resolvedCoverage[i].commands.first();
      if (command.commandIndex != first.commandIndex ||
          child.range.entry.drawParamIndex != expectedDrawParam) {
        return result;
      }
      if (expectedDrawParam > UINT32_MAX - child.range.drawEntryCount) {
        return result;
      }
      expectedDrawParam += child.range.drawEntryCount;
    }
    if (expectedDrawParam !=
        static_cast<std::uint64_t>(first.drawParamBegin) +
            first.drawParamCount) {
      return result;
    }
  }
  result.failure = ParallelPassSemanticPlanFailure::ChildPlan;
  if (validateParallelPassChildPlans(children) !=
      ParallelPassFallbackReason::None) {
    return result;
  }
  result.failure = ParallelPassSemanticPlanFailure::Coverage;
  if (!validateParallelPassSemanticPlanCoverage(
          *snapshot, children,
          {resolvedCoverage.data(), children.size()})) {
    return result;
  }
  result.failure = ParallelPassSemanticPlanFailure::None;
  result.plan.snapshot_ = *snapshot;
  std::copy(children.begin(), children.end(), result.plan.children_.begin());
  result.plan.childCount_ = static_cast<std::uint32_t>(children.size());
  return result;
}

inline ParallelPassSemanticPlanValidation validateParallelPassSemanticPlan(
    const SealedParallelPassSnapshot& snapshot,
    std::span<const ParallelPassChildPlan> children,
    const ParallelPassSnapshotAuthority& authority,
    const ParallelPassCoverageResolver& resolver,
    const ParallelPassActionEpochWitness& epochWitness) noexcept {
  return validateParallelPassSemanticPlan(&snapshot, children, authority,
                                          resolver, epochWitness);
}

struct ParallelPassFixedPoint {
  static constexpr std::int64_t kFraction = 1ll << 16;
  static constexpr std::int64_t kMaxRaw = (1ll << 48) - 1ll;
  std::int64_t raw = 0;
  friend constexpr bool operator==(const ParallelPassFixedPoint&,
                                   const ParallelPassFixedPoint&) = default;
};

inline bool parallelPassFixedPointFromUnsigned(
    std::uint64_t value, ParallelPassFixedPoint& result) noexcept {
  if (value > static_cast<std::uint64_t>(
                  ParallelPassFixedPoint::kMaxRaw) /
                  static_cast<std::uint64_t>(ParallelPassFixedPoint::kFraction)) {
    return false;
  }
  const auto raw = static_cast<__int128>(value) *
      ParallelPassFixedPoint::kFraction;
  if (raw > ParallelPassFixedPoint::kMaxRaw) {
    return false;
  }
  result.raw = static_cast<std::int64_t>(raw);
  return true;
}

inline constexpr bool parallelPassFixedPointInCostDomain(
    ParallelPassFixedPoint value) noexcept {
  return value.raw >= 0 && value.raw <= ParallelPassFixedPoint::kMaxRaw;
}

inline bool parallelPassFixedPointAdd(ParallelPassFixedPoint left,
                                       ParallelPassFixedPoint right,
                                       ParallelPassFixedPoint& result) noexcept {
  if (!parallelPassFixedPointInCostDomain(left) ||
      !parallelPassFixedPointInCostDomain(right)) {
    return false;
  }
  if ((right.raw > 0 && left.raw >
           std::numeric_limits<std::int64_t>::max() - right.raw) ||
      (right.raw < 0 && left.raw <
           std::numeric_limits<std::int64_t>::min() - right.raw)) {
    return false;
  }
  result.raw = left.raw + right.raw;
  return parallelPassFixedPointInCostDomain(result);
}

inline bool parallelPassFixedPointSubtract(
    ParallelPassFixedPoint left, ParallelPassFixedPoint right,
    ParallelPassFixedPoint& result) noexcept {
  if (!parallelPassFixedPointInCostDomain(left) ||
      !parallelPassFixedPointInCostDomain(right)) {
    return false;
  }
  if ((right.raw < 0 && left.raw >
           std::numeric_limits<std::int64_t>::max() + right.raw) ||
      (right.raw > 0 && left.raw <
           std::numeric_limits<std::int64_t>::min() + right.raw)) {
    return false;
  }
  result.raw = left.raw - right.raw;
  return result.raw >= -ParallelPassFixedPoint::kMaxRaw &&
      result.raw <= ParallelPassFixedPoint::kMaxRaw;
}

inline bool parallelPassFixedPointMultiply(
    ParallelPassFixedPoint left, ParallelPassFixedPoint right,
    ParallelPassFixedPoint& result) noexcept {
  if (!parallelPassFixedPointInCostDomain(left) ||
      !parallelPassFixedPointInCostDomain(right)) {
    return false;
  }
  const __int128 product = static_cast<__int128>(left.raw) * right.raw;
  const __int128 scaled = product / ParallelPassFixedPoint::kFraction;
  if (scaled > std::numeric_limits<std::int64_t>::max() ||
      scaled < std::numeric_limits<std::int64_t>::min()) {
    return false;
  }
  result.raw = static_cast<std::int64_t>(scaled);
  return parallelPassFixedPointInCostDomain(result);
}

struct ParallelPassCandidateCost {
  ParallelPassEconomicsSummary economics{};
  ParallelPassFixedPoint serialWork{};
  ParallelPassFixedPoint criticalPath{};
  ParallelPassFixedPoint childSetup{};
  ParallelPassFixedPoint imbalance{};
  bool valid = false;
  bool overflow = false;
};

inline bool validateParallelPassStructuralEconomics(
    const ParallelPassSemanticPlanView& plan,
    const ParallelPassCandidateCost& cost) noexcept {
  if (!plan.valid() || !cost.valid || cost.overflow ||
      !parallelPassFixedPointInCostDomain(cost.serialWork) ||
      !parallelPassFixedPointInCostDomain(cost.criticalPath) ||
      !parallelPassFixedPointInCostDomain(cost.childSetup) ||
      !parallelPassFixedPointInCostDomain(cost.imbalance)) {
    return false;
  }
  const auto& economics = cost.economics;
  constexpr std::uint64_t maxDraws = 1ull << 48;
  if (!economics.valid || economics.overflow ||
      economics.totalDraws == 0u || economics.totalDraws > maxDraws ||
      economics.childCount != plan.children().size() ||
      economics.childCount < 2u ||
      economics.childCount > kParallelRenderPassChildCapacity ||
      economics.minimumChildDraws == 0u ||
      economics.maximumChildDraws < economics.minimumChildDraws) {
    return false;
  }
  if (plan.snapshot().drawCount > maxDraws) {
    return false;
  }
  const auto checkedProduct = [](std::uint64_t left, std::uint64_t right,
                                 std::uint64_t& product) noexcept {
    if (right != 0u && left > std::numeric_limits<std::uint64_t>::max() /
                              right) {
      return false;
    }
    product = left * right;
    return true;
  };
  std::uint64_t minimumCoveredDraws = 0u;
  std::uint64_t maximumCoveredDraws = 0u;
  if (!checkedProduct(economics.childCount, economics.minimumChildDraws,
                      minimumCoveredDraws) ||
      !checkedProduct(economics.childCount, economics.maximumChildDraws,
                      maximumCoveredDraws) ||
      minimumCoveredDraws > maxDraws || maximumCoveredDraws > maxDraws) {
    return false;
  }
  if (economics.totalDraws != plan.snapshot().drawCount ||
      economics.totalDraws < minimumCoveredDraws ||
      economics.totalDraws > maximumCoveredDraws ||
      economics.stage1Draws > economics.totalDraws ||
      economics.stage2bDraws > economics.totalDraws - economics.stage1Draws ||
      economics.forcedStage1Draws != economics.totalDraws -
          economics.stage1Draws - economics.stage2bDraws ||
      economics.psoBoundaryTransitions > economics.totalDraws ||
      economics.uniformBoundaryTransitions > economics.totalDraws) {
    return false;
  }
  return true;
}

struct ParallelPassRangeTieKey {
  std::uint32_t sourceIndex = 0;
  std::uint64_t sourceGeneration = 0;
  std::uint32_t firstPage = 0;
  std::uint32_t pageCount = 0;
  std::uint64_t storageGeneration = 0;
  std::uint32_t retainedSourceIndex = 0;
  std::uint32_t slotIndex = 0;
  std::uint64_t seqId = 0;
  std::uint32_t replayOrdinal = 0;
  std::uint32_t replayOrdinalCount = 0;
  std::uint32_t rangeKind = 0;
  std::uint32_t commandIndex = 0;
  std::uint32_t drawParamIndex = 0;
  std::uint32_t drawEntryCount = 0;
  std::uint32_t drawRunRecordIndex = 0;
  std::uint32_t stateIndex = 0;
  std::uint32_t uniformIndex = 0;
  std::uint32_t uniformGeneration = 0;
  std::uint64_t uniformHash = 0;
  std::uint32_t bindingOverrideOffset = 0;
  std::uint32_t bindingOverrideSize = 0;
  std::uint32_t bindingSnapshotOffset = 0;
  std::uint32_t bindingSnapshotSize = 0;
  friend constexpr bool operator==(const ParallelPassRangeTieKey&,
                                   const ParallelPassRangeTieKey&) = default;
};

struct ParallelPassCandidateScore {
  ParallelPassFixedPoint benefit{};
  std::array<ParallelPassRangeTieKey,
             kParallelRenderPassChildCapacity> ranges{};
  std::uint32_t rangeCount = 0;
  std::uint32_t childCount = 0;
  std::uint64_t totalDraws = 0;
  std::uint32_t candidateOrdinal = UINT32_MAX;
  bool valid = false;

  friend constexpr bool operator==(const ParallelPassCandidateScore&,
                                   const ParallelPassCandidateScore&) = default;
};

enum class ParallelPassCandidateSelectionFailure : std::uint8_t {
  None,
  Empty,
  InvalidPlan,
  InvalidEconomics,
  NonPositiveBenefit,
  Arithmetic,
  InvalidCandidateOrdinal,
  Count,
};

struct ParallelPassCandidateInput {
  const SealedParallelPassSnapshot* snapshot = nullptr;
  std::span<const ParallelPassChildPlan> children{};
  ParallelPassCandidateCost cost{};
  ParallelPassSnapshotAuthority authority{};
  ParallelPassCoverageResolver coverage{};
  ParallelPassActionEpochWitness epochWitness{};
  std::uint32_t candidateOrdinal = UINT32_MAX;
};

struct ParallelPassCandidateSelection {
  ParallelPassCandidateSelectionFailure failure =
      ParallelPassCandidateSelectionFailure::Empty;
  std::uint32_t candidateOrdinal = UINT32_MAX;
  ParallelPassCandidateScore score{};
  bool selected = false;
};

inline ParallelPassCandidateScore scoreParallelPassCandidate(
    const ParallelPassSemanticPlanView& plan,
    const ParallelPassCandidateCost& cost,
    std::uint32_t candidateOrdinal) noexcept {
  ParallelPassCandidateScore result{.candidateOrdinal = candidateOrdinal};
  if (!validateParallelPassStructuralEconomics(plan, cost)) {
    return result;
  }
  ParallelPassFixedPoint totalCost{};
  if (!parallelPassFixedPointAdd(cost.criticalPath, cost.childSetup,
                                 totalCost) ||
      !parallelPassFixedPointAdd(totalCost, cost.imbalance, totalCost)) {
    return result;
  }
  ParallelPassFixedPoint benefit = cost.serialWork;
  if (!parallelPassFixedPointSubtract(benefit, totalCost, benefit)) {
    return result;
  }
  result.benefit = benefit;
  result.childCount = static_cast<std::uint32_t>(plan.children().size());
  result.totalDraws = plan.snapshot().drawCount;
  result.rangeCount = result.childCount;
  for (std::size_t i = 0u; i < result.rangeCount; ++i) {
    const auto& range = plan.children()[i].range;
    const auto& source = range.entry.source;
    result.ranges[i] = ParallelPassRangeTieKey{
        .sourceIndex = source.tapeSource.id.index,
        .sourceGeneration = source.tapeSource.id.generation,
        .firstPage = source.tapeSource.storage.firstPage,
        .pageCount = source.tapeSource.storage.pageCount,
        .storageGeneration = source.tapeSource.storage.generation,
        .retainedSourceIndex = source.retainedSourceIndex,
        .slotIndex = source.slotIndex,
        .seqId = source.seqId,
        .replayOrdinal = range.replayOrdinalBegin,
        .replayOrdinalCount = range.replayOrdinalCount,
        .rangeKind = static_cast<std::uint32_t>(range.kind),
        .commandIndex = range.entry.commandIndex,
        .drawParamIndex = range.entry.drawParamIndex,
        .drawEntryCount = range.drawEntryCount,
        .drawRunRecordIndex = range.entry.drawRunRecordIndex,
        .stateIndex = range.entry.stateIndex,
        .uniformIndex = range.entry.uniformHandle.index,
        .uniformGeneration = range.entry.uniformHandle.generation,
        .uniformHash = range.entry.uniformHandle.hash,
        .bindingOverrideOffset = range.entry.bindingOverrideBytes.offset,
        .bindingOverrideSize = range.entry.bindingOverrideBytes.size,
        .bindingSnapshotOffset = range.entry.bindingSnapshotBytes.offset,
        .bindingSnapshotSize = range.entry.bindingSnapshotBytes.size,
    };
  }
  result.valid = true;
  return result;
}

inline constexpr bool parallelPassRangeTieKeyLess(
    const ParallelPassRangeTieKey& left,
    const ParallelPassRangeTieKey& right) noexcept {
  const auto tupleLeft = std::tuple{left.sourceIndex, left.sourceGeneration,
                                    left.firstPage, left.pageCount,
                                    left.storageGeneration,
                                    left.retainedSourceIndex, left.slotIndex,
                                    left.seqId, left.replayOrdinal,
                                    left.replayOrdinalCount, left.rangeKind,
                                    left.commandIndex, left.drawParamIndex,
                                    left.drawEntryCount,
                                    left.drawRunRecordIndex, left.stateIndex,
                                    left.uniformIndex, left.uniformGeneration,
                                    left.uniformHash, left.bindingOverrideOffset,
                                    left.bindingOverrideSize,
                                    left.bindingSnapshotOffset,
                                    left.bindingSnapshotSize};
  const auto tupleRight = std::tuple{right.sourceIndex, right.sourceGeneration,
                                     right.firstPage, right.pageCount,
                                     right.storageGeneration,
                                     right.retainedSourceIndex, right.slotIndex,
                                     right.seqId, right.replayOrdinal,
                                     right.replayOrdinalCount, right.rangeKind,
                                     right.commandIndex, right.drawParamIndex,
                                     right.drawEntryCount,
                                     right.drawRunRecordIndex, right.stateIndex,
                                     right.uniformIndex,
                                     right.uniformGeneration, right.uniformHash,
                                     right.bindingOverrideOffset,
                                     right.bindingOverrideSize,
                                     right.bindingSnapshotOffset,
                                     right.bindingSnapshotSize};
  return tupleLeft < tupleRight;
}

inline constexpr bool parallelPassCandidateScoreLess(
    const ParallelPassCandidateScore& left,
    const ParallelPassCandidateScore& right) noexcept {
  if (left.benefit.raw != right.benefit.raw) {
    return left.benefit.raw > right.benefit.raw;
  }
  if (left.childCount != right.childCount) {
    return left.childCount < right.childCount;
  }
  for (std::size_t i = 0u; i < std::min(left.rangeCount, right.rangeCount); ++i) {
    if (left.ranges[i] == right.ranges[i]) {
      continue;
    }
    return parallelPassRangeTieKeyLess(left.ranges[i], right.ranges[i]);
  }
  if (left.rangeCount != right.rangeCount) {
    return left.rangeCount < right.rangeCount;
  }
  return left.candidateOrdinal < right.candidateOrdinal;
}

inline ParallelPassCandidateSelection selectParallelPassCandidate(
    std::span<const ParallelPassCandidateInput> candidates) noexcept {
  ParallelPassCandidateSelection result{};
  if (candidates.empty() ||
      candidates.size() > kParallelRenderPassCandidateCapacity) {
    return result;
  }
  std::array<std::uint32_t, kParallelRenderPassCandidateCapacity> ordinals{};
  for (std::size_t i = 0u; i < candidates.size(); ++i) {
    if (candidates[i].candidateOrdinal == UINT32_MAX ||
        candidates[i].candidateOrdinal >=
            kParallelRenderPassCandidateCapacity ||
        std::find(ordinals.begin(), ordinals.begin() + i,
                  candidates[i].candidateOrdinal) != ordinals.begin() + i) {
      result.failure = ParallelPassCandidateSelectionFailure::InvalidCandidateOrdinal;
      return result;
    }
    ordinals[i] = candidates[i].candidateOrdinal;
  }
  bool haveBest = false;
  ParallelPassCandidateScore best{};
  for (const auto& candidate : candidates) {
    const auto validation = validateParallelPassSemanticPlan(
        candidate.snapshot, candidate.children, candidate.authority,
        candidate.coverage, candidate.epochWitness);
    if (!validation.accepted()) {
      result.failure = ParallelPassCandidateSelectionFailure::InvalidPlan;
      return result;
    }
    const auto score = scoreParallelPassCandidate(
        validation.plan, candidate.cost, candidate.candidateOrdinal);
    if (!score.valid) {
      result.failure = ParallelPassCandidateSelectionFailure::InvalidEconomics;
      return result;
    }
    if (score.benefit.raw <= 0) {
      continue;
    }
    if (!haveBest || parallelPassCandidateScoreLess(score, best)) {
      best = score;
      haveBest = true;
    }
  }
  if (!haveBest) {
    result.failure = ParallelPassCandidateSelectionFailure::NonPositiveBenefit;
    return result;
  }
  result.failure = ParallelPassCandidateSelectionFailure::None;
  result.candidateOrdinal = best.candidateOrdinal;
  result.score = best;
  result.selected = true;
  return result;
}

// Deterministic bounded cost record derived only from integers the certified
// plan already carries, so an identical plan always scores identically
// regardless of evaluation order or worker scheduling (`R-BACK-2.72`). Serial
// work is every draw; the parallel critical path is the widest child; each
// child pays one draw-equivalent first-bind reset; residual imbalance is the
// widest child minus the narrowest. Any conversion overflow leaves the record
// invalid, which selects serial.
inline bool buildParallelPassCandidateCost(
    const ParallelPassEconomicsSummary& economics,
    ParallelPassCandidateCost& cost) noexcept {
  cost = {};
  cost.economics = economics;
  if (!economics.valid || economics.overflow || economics.childCount < 2u ||
      economics.childCount > kParallelRenderPassChildCapacity ||
      economics.minimumChildDraws == 0u ||
      economics.maximumChildDraws < economics.minimumChildDraws ||
      economics.maximumChildDraws > economics.totalDraws) {
    cost.overflow = true;
    return false;
  }
  if (!parallelPassFixedPointFromUnsigned(economics.totalDraws,
                                          cost.serialWork) ||
      !parallelPassFixedPointFromUnsigned(economics.maximumChildDraws,
                                          cost.criticalPath) ||
      !parallelPassFixedPointFromUnsigned(economics.childCount,
                                          cost.childSetup) ||
      !parallelPassFixedPointFromUnsigned(
          economics.maximumChildDraws - economics.minimumChildDraws,
          cost.imbalance)) {
    cost.overflow = true;
    return false;
  }
  cost.valid = true;
  return true;
}

enum class ParallelPassAdapterOutcome : std::uint8_t {
  CertificateInvalid,
  NotSelected,
  Selected,
};

struct ParallelPassAdapterDecision {
  ParallelPassAdapterOutcome outcome =
      ParallelPassAdapterOutcome::CertificateInvalid;
  ParallelPassSemanticPlanFailure certificate =
      ParallelPassSemanticPlanFailure::MissingSnapshot;
  ParallelPassCandidateSelectionFailure selection =
      ParallelPassCandidateSelectionFailure::Empty;
  std::uint32_t candidateOrdinal = UINT32_MAX;

  constexpr bool certificateValid() const noexcept {
    return outcome != ParallelPassAdapterOutcome::CertificateInvalid;
  }
  constexpr bool selected() const noexcept {
    return outcome == ParallelPassAdapterOutcome::Selected;
  }

  friend constexpr bool operator==(const ParallelPassAdapterDecision&,
                                   const ParallelPassAdapterDecision&) =
      default;
};

struct ParallelPassAdapterAccounting {
  std::uint64_t considered = 0;
  std::uint64_t certificateValid = 0;
  std::uint64_t certificateInvalid = 0;
  std::uint64_t selected = 0;
  std::uint64_t serialFallback = 0;

  constexpr bool conserves() const noexcept {
    return considered == certificateValid + certificateInvalid &&
        considered == selected + serialFallback &&
        selected <= certificateValid;
  }
};

inline constexpr ParallelPassAdapterAccounting accountParallelPassAdapter(
    const ParallelPassAdapterDecision& decision) noexcept {
  ParallelPassAdapterAccounting result{.considered = 1u};
  if (decision.certificateValid()) {
    result.certificateValid = 1u;
  } else {
    result.certificateInvalid = 1u;
  }
  if (decision.selected()) {
    result.selected = 1u;
  } else {
    result.serialFallback = 1u;
  }
  return result;
}

// The single production entry into the policy proof core. It runs before any
// Metal effect: an invalid certificate can never reach the selector, and an
// unselected candidate can never reach child creation. The certificate is
// consumed whole — a partial result is never carried forward — and the
// selector independently revalidates through the same owner-issued authority
// and exact coverage resolver (`R-BACK-2.69`, `R-BACK-2.74`).
inline ParallelPassAdapterDecision runParallelPassProofCoreAdapter(
    const SealedParallelPassSnapshot& snapshot,
    std::span<const ParallelPassChildPlan> children,
    const ParallelPassCandidateCost& cost,
    const ParallelPassSnapshotAuthority& authority,
    const ParallelPassCoverageResolver& coverage,
    const ParallelPassActionEpochWitness& epochWitness) noexcept {
  ParallelPassAdapterDecision result{};
  const auto certificate = validateParallelPassSemanticPlan(
      &snapshot, children, authority, coverage, epochWitness);
  result.certificate = certificate.failure;
  if (!certificate.accepted()) {
    return result;
  }
  result.outcome = ParallelPassAdapterOutcome::NotSelected;
  const std::array<ParallelPassCandidateInput, 1> candidates{
      ParallelPassCandidateInput{
          .snapshot = &snapshot,
          .children = children,
          .cost = cost,
          .authority = authority,
          .coverage = coverage,
          .epochWitness = epochWitness,
          .candidateOrdinal = 0u,
      }};
  const auto selection = selectParallelPassCandidate(candidates);
  result.selection = selection.failure;
  if (!selection.selected || selection.candidateOrdinal != 0u) {
    return result;
  }
  result.outcome = ParallelPassAdapterOutcome::Selected;
  result.candidateOrdinal = selection.candidateOrdinal;
  return result;
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
static_assert(std::is_trivially_copyable_v<ParallelPassFixedPoint>);
static_assert(std::is_standard_layout_v<ParallelPassFixedPoint>);
static_assert(std::is_trivially_copyable_v<ParallelPassCandidateCost>);
static_assert(std::is_standard_layout_v<ParallelPassCandidateCost>);
static_assert(std::is_trivially_copyable_v<ParallelPassCandidateScore>);
static_assert(std::is_standard_layout_v<ParallelPassCandidateScore>);
static_assert(std::is_trivially_copyable_v<ParallelPassCandidateSelection>);
static_assert(std::is_standard_layout_v<ParallelPassCandidateSelection>);
static_assert(std::is_trivially_copyable_v<ParallelPassSemanticPlanView>);
static_assert(std::is_standard_layout_v<ParallelPassSemanticPlanView>);
static_assert(std::is_trivially_copyable_v<ParallelPassResolvedCoverage>);
static_assert(std::is_standard_layout_v<ParallelPassResolvedCoverage>);
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
