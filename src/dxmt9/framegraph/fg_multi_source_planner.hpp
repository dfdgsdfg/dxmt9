#pragma once

// Device-free, bounded passcoalesce planner over an already-Represented
// ready-source window. SourcePayloadView borrows remain call-local; the output
// retains only compact indices into the caller-owned retained-source array.

#include "fg_builder.hpp"
#include "../dxmt9_cpu_ready_tape.hpp"
#include "../dxmt9_encode_attribution.hpp"

#include <array>
#include <limits>
#include <span>
#include <type_traits>
#include <vector>

namespace dxmt9::framegraph {

inline constexpr std::size_t kMaxMultiSourcePlanningSources = 8u;

struct MultiSourcePlanningSource {
  core::SourcePayloadView payload{};
};

// Builds one event-wide DAG over the immutable source window.  Command and
// pass indices are rebased into one graph and hazards are re-inferred after
// all rows have been appended; callers must not substitute source-local
// graphs when authenticating a pass that crosses a source boundary.
bool buildMultiSourceFrameGraph(
    std::span<const MultiSourcePlanningSource> sources,
    FrameGraph& out) noexcept;

// Borrowed call-local input for the exact deferred terminal-suffix lane. The
// payload must never be copied into the returned proof; source identity and
// command ranges below are the only values that may survive planning.
struct DeferredTerminalSuffixPlanningSourceView {
  core::SourcePayloadView payload{};
  core::CpuReadyTape::SourceRef source{};
  u64 sourceOrdinal = 0;
  u64 seqId = 0;
};

struct SourceCommandRange {
  core::CpuReadyTape::SourceRef source{};
  u64 sourceOrdinal = 0;
  u64 seqId = 0;
  u32 commandBegin = 0;
  u32 commandCount = 0;

  constexpr bool valid() const noexcept {
    return source.valid() && sourceOrdinal != 0 && seqId != 0 &&
           commandCount != 0 &&
           commandBegin <= std::numeric_limits<u32>::max() - commandCount;
  }

  friend constexpr bool operator==(const SourceCommandRange&,
                                   const SourceCommandRange&) = default;
};

struct RetainedSourceCommandLocator {
  u32 retainedSourceIndex = 0;
  u32 commandIndex = 0;

  friend bool operator==(const RetainedSourceCommandLocator&,
                         const RetainedSourceCommandLocator&) = default;
};

inline constexpr std::size_t kDeferredTerminalSuffixSourceCount = 2u;
inline constexpr std::size_t kDeferredTerminalSuffixCommandCount = 4u;

// Pointer-free proof for exactly A,Clear(B),DrawRun(B) | DrawRun(A). The
// joined order moves only the younger returning-A DrawRun ahead of the older
// terminal suffix; currentPrefix has already encoded when the coordinator
// consumes this proof.
struct DeferredTerminalSuffixProof {
  SourceCommandRange currentPrefix{};
  SourceCommandRange currentSuffix{};
  SourceCommandRange successorHead{};
  ActiveRenderPlanningSeed activeRender{};
  AttachmentSet currentAttachmentA{};
  AttachmentSet clearAttachmentB{};
  std::array<RetainedSourceCommandLocator,
             kDeferredTerminalSuffixCommandCount>
      naturalReplay{};
  std::array<RetainedSourceCommandLocator,
             kDeferredTerminalSuffixCommandCount>
      joinedReplay{};
};

enum class DeferredTerminalSuffixDisposition : u8 {
  InvalidInput,
  StaleIdentity,
  UnsupportedShape,
  UnsupportedBoundary,
  MalformedRange,
  AttachmentMismatch,
  DependencyWedged,
  IncompleteCoverage,
  Qualified,
};

struct DeferredTerminalSuffixPlan {
  DeferredTerminalSuffixProof proof{};
  DeferredTerminalSuffixDisposition disposition =
      DeferredTerminalSuffixDisposition::InvalidInput;

  constexpr bool qualified() const noexcept {
    return disposition == DeferredTerminalSuffixDisposition::Qualified;
  }
};

enum class DeferredTerminalSuffixReplayValidation : u8 {
  Valid,
  InvalidProof,
  StaleIdentity,
  InvalidSource,
  InvalidCommand,
  Duplicate,
  Missing,
  UnsupportedMovement,
};

static_assert(std::is_trivially_copyable_v<SourceCommandRange>);
static_assert(std::is_standard_layout_v<SourceCommandRange>);
static_assert(std::is_trivially_copyable_v<DeferredTerminalSuffixProof>);
static_assert(std::is_standard_layout_v<DeferredTerminalSuffixProof>);
static_assert(std::is_trivially_copyable_v<DeferredTerminalSuffixPlan>);
static_assert(std::is_standard_layout_v<DeferredTerminalSuffixPlan>);
static_assert(sizeof(DeferredTerminalSuffixPlan) <= 1024u,
              "deferred suffix proof must remain a bounded value");

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

// Classify and prove only the measured terminal suffix shape. FrameGraph and
// payload borrows are call-local; the returned plan is a bounded value. The
// general multi-source permutation validator is intentionally not involved.
DeferredTerminalSuffixPlan planDeferredTerminalSuffixReplay(
    const DeferredTerminalSuffixPlanningSourceView& current,
    const DeferredTerminalSuffixPlanningSourceView& successor,
    const ActiveRenderPlanningSeed& activeRender,
    ResourceAliasResolver aliasResolver = {}) noexcept;

// Narrow pre-effect validator for the proof above. It reclassifies the exact
// generation-stamped sources and accepts only proof.joinedReplay with complete
// four-command coverage.
DeferredTerminalSuffixReplayValidation validateDeferredTerminalSuffixReplay(
    const DeferredTerminalSuffixPlanningSourceView& current,
    const DeferredTerminalSuffixPlanningSourceView& successor,
    const ActiveRenderPlanningSeed& activeRender,
    const DeferredTerminalSuffixProof& proof,
    std::span<const RetainedSourceCommandLocator> commands,
    ResourceAliasResolver aliasResolver = {}) noexcept;

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
