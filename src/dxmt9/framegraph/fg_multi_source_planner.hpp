#pragma once

// Device-free, bounded passcoalesce planner over an already-Represented
// ready-source window. SourcePayloadView borrows remain call-local; the output
// retains only compact indices into the caller-owned retained-source array.

#include "fg_builder.hpp"
#include "../dxmt9_encode_attribution.hpp"

#include <span>
#include <vector>

namespace dxmt9::framegraph {

inline constexpr std::size_t kMaxMultiSourcePlanningSources = 8u;

struct MultiSourcePlanningSource {
  core::SourcePayloadView payload{};
};

struct RetainedSourceCommandLocator {
  u32 retainedSourceIndex = 0;
  u32 commandIndex = 0;

  friend bool operator==(const RetainedSourceCommandLocator&,
                         const RetainedSourceCommandLocator&) = default;
};

enum class MultiSourceReplayValidation : u8 {
  Valid,
  InvalidSource,
  InvalidCommand,
  Duplicate,
  Missing,
  NonDrawCrossSourceMovement,
};

enum class MultiSourceReplayDisposition : u8 {
  Planned,
  NaturalFifo,
  InvalidInput,
};

enum class MultiSourceSeedApplyDiagnostic : u8 {
  NotRequested,
  Applied,
  Invalid,
  Incomplete,
  Overflow,
};

enum class MultiSourceActiveTargetMatchDiagnostic : u8 {
  NotRequested,
  Absent,
  Present,
};

enum class MultiSourceFirstMatchingCommandDiagnostic : u8 {
  None,
  DrawRun,
  NonDraw,
};

enum class MultiSourceMergeDiagnostic : u8 {
  None,
  SeedMerged,
  NonSeedOnly,
};

// Exclusive terminal classification. The remaining value fields retain the
// orthogonal mechanism detail without logging or heap-owning diagnostics.
enum class MultiSourcePlannerOutcome : u8 {
  InvalidInput,
  SeedRejected,
  NoActiveTargetMatch,
  NoMerge,
  NaturalAfterMerge,
  PermutationRejected,
  MovedHeadUnproved,
  Planned,
};

struct MultiSourceReplayDiagnostics {
  MultiSourceSeedApplyDiagnostic seedApply =
      MultiSourceSeedApplyDiagnostic::NotRequested;
  MultiSourceActiveTargetMatchDiagnostic activeTargetMatch =
      MultiSourceActiveTargetMatchDiagnostic::NotRequested;
  MultiSourceFirstMatchingCommandDiagnostic firstMatchingCommand =
      MultiSourceFirstMatchingCommandDiagnostic::None;
  MultiSourceMergeDiagnostic merge = MultiSourceMergeDiagnostic::None;
  MultiSourcePlannerOutcome outcome =
      MultiSourcePlannerOutcome::InvalidInput;
  u32 firstMatchingPassDistance = 0;
  // Actual successful fixpoint merge attribution, distinct from the first
  // pre-optimizer target match above. Totals make multi-merge cancellation and
  // commandless-intervening invariants observable without retaining the DAG.
  u32 optimizerMergeCount = 0;
  u32 activeSeedMergeCount = 0;
  u64 activeSeedMergeDistanceTotal = 0;
  u32 activeSeedMergeDistanceMax = 0;
  u64 activeSeedCommandBefore = 0;
  u64 activeSeedCommandAfter = 0;
  u64 activeSeedEmptyIntervening = 0;
  bool seedSecondNonDraw = false;
  bool seedBlockedCycle = false;
  bool interveningNonRender = false;
  bool activeSeedMergeAttributionMissing = false;
  std::vector<encoders::ActiveSeedMergeTargetWitness>
      activeSeedMergeWitnesses;
  bool activeSeedMergeWitnessOverflow = false;
  bool activeSeedMergeWitnessMismatch = false;
};

struct MultiSourceReplayPlan {
  std::vector<RetainedSourceCommandLocator> commands;
  MultiSourceReplayDisposition disposition =
      MultiSourceReplayDisposition::InvalidInput;
  MultiSourceReplayValidation validation =
      MultiSourceReplayValidation::InvalidSource;
  MultiSourceReplayDiagnostics diagnostics{};

  bool valid() const noexcept {
    return disposition != MultiSourceReplayDisposition::InvalidInput &&
           validation == MultiSourceReplayValidation::Valid;
  }
  bool reordered() const noexcept {
    return disposition == MultiSourceReplayDisposition::Planned;
  }
};

// One maximal contiguous source-local range in a qualified replay plan.
// sourceFragmentOrdinal/sourceFragmentCount identify this run within all runs
// for the same retained source. They let the serial executor run source-wide
// setup exactly once even when qualified replay revisits a source as A|B|A.
struct MultiSourceReplayRun {
  u32 retainedSourceIndex = 0;
  u32 commandBegin = 0;
  u32 commandCount = 0;
  u32 sourceFragmentOrdinal = 0;
  u32 sourceFragmentCount = 1;
  u32 transactionFragmentOrdinal = 0;
  u32 transactionFragmentCount = 1;

  constexpr bool firstSourceFragment() const noexcept {
    return sourceFragmentOrdinal == 0u;
  }

  constexpr bool lastSourceFragment() const noexcept {
    return sourceFragmentCount != 0u &&
           sourceFragmentOrdinal + 1u == sourceFragmentCount;
  }

  constexpr bool firstTransactionFragment() const noexcept {
    return transactionFragmentOrdinal == 0u;
  }

  friend bool operator==(const MultiSourceReplayRun&,
                         const MultiSourceReplayRun&) = default;
};

enum class MultiSourceReplayRunValidation : u8 {
  Valid,
  InvalidPlan,
  // Retained for counter/schema compatibility. The fragment-aware executor
  // now accepts repeated source runs after exact-plan validation.
  RepeatedSourceRun,
};

struct MultiSourceReplayRuns {
  std::vector<MultiSourceReplayRun> runs;
  MultiSourceReplayRunValidation validation =
      MultiSourceReplayRunValidation::InvalidPlan;

  bool valid() const noexcept {
    return validation == MultiSourceReplayRunValidation::Valid;
  }
};

// Validate an exact, complete permutation of the retained source window.
// Cross-source inversions are accepted only when both commands are DrawRun.
// This is the fail-open seam used before any future Metal executor consumes a
// plan.
MultiSourceReplayValidation validateMultiSourceReplayPermutation(
    std::span<const MultiSourcePlanningSource> sources,
    std::span<const RetainedSourceCommandLocator> commands);

// Concatenate source-local graphs, re-infer hazards across source boundaries,
// apply one optional active-render seed to the combined graph, and run only
// passcoalesce. A rejected optimization returns a complete NaturalFifo plan;
// malformed source input returns InvalidInput and no command plan.
MultiSourceReplayPlan planMultiSourcePassCoalesceReplay(
    std::span<const MultiSourcePlanningSource> sources,
    const ActiveRenderPlanningSeed* activeRenderSeed = nullptr,
    ResourceAliasResolver aliasResolver = {},
    bool collectActiveSeedMergeWitnesses = false);

// Convert a validated plan into maximal contiguous per-source command runs.
// The transform is pure and fail-closed: malformed or stale plans produce
// nullopt rather than a partially executable prefix.
MultiSourceReplayRuns buildMultiSourceReplayRuns(
    std::span<const MultiSourcePlanningSource> sources,
    const MultiSourceReplayPlan& plan);

}  // namespace dxmt9::framegraph
