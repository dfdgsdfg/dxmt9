#pragma once

#include "dxmt9_encode_session.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>

namespace dxmt9::encoders {

// A resolved entry borrows queue-owned source storage only for the synchronous
// encode call. It must never be retained by EncodeSession or a Metal callback.
struct ResolvedEncodePartitionEntry {
  // Compatibility locator for legacy ChunkSlot sources. Arena sources leave
  // this null; all consumers must use the command-local spans below.
  const core::ChunkSlot* slot = nullptr;
  core::MetalCommandView command{};
  const core::DrawRunCommandRecord* drawRunRecord = nullptr;
  core::FlatDrawStateView drawState{};
  const core::DrawParam* drawParam = nullptr;
  const core::DrawUniformPayloadRecord* uniform = nullptr;
  std::span<const core::u8> bindingOverrideBytes{};
  std::span<const core::u8> bindingSnapshotBytes{};
};

// A resolved partition is a borrowed current-call view. The entry and
// DrawParam span must be consumed synchronously and must never be retained by
// an EncodeSession, submission record, or Metal callback.
struct ResolvedEncodePartition {
  ResolvedEncodePartitionEntry entry{};
  std::span<const core::DrawParam> drawParams{};
};

enum class EncodePartitionEntryValidation {
  Valid,
  RetainedSourceIndexOutOfRange,
  SourceIdentityMismatch,
  SourceCommandRangeInvalid,
  CommandIndexOutOfRange,
  CommandIsNotDrawRun,
  DrawRunRecordMismatch,
  StateIndexMismatch,
  DrawParamIndexMismatch,
  UniformHandleMismatch,
  BindingOverrideRangeMismatch,
  BindingSnapshotRangeMismatch,
};

struct EncodePartitionEntryResolution {
  EncodePartitionEntryValidation validation =
      EncodePartitionEntryValidation::RetainedSourceIndexOutOfRange;
  ResolvedEncodePartitionEntry entry{};

  explicit operator bool() const noexcept {
    return validation == EncodePartitionEntryValidation::Valid;
  }
};

// Resolves locator-only partition metadata against the call-local retained
// source table. Every identity, command, segment-local SoA index, uniform
// handle, and payload range must agree with immutable SourcePayloadView
// storage. Rejection is side-effect free so the caller can fail open to
// source-order serial encoding.
EncodePartitionEntryResolution resolveEncodePartitionEntry(
    const EncodePartitionEntrySnapshot& snapshot,
    std::span<const core::metalqueue::ResolvedPublishedSource> sources) noexcept;

// Exact current-source view of the already selected effective replay stream.
// `source` is by value, but its ChunkSlot pointer and command-order span are
// borrowed only for the synchronous encode call.
struct EncodePartitionReplayStream {
  core::metalqueue::ResolvedPublishedSource source{};
  bool commandOrderActive = false;
  std::span<const std::uint32_t> commandOrder{};
  bool valid = false;

  std::size_t replayOrdinalCount() const noexcept;
  bool commandIndexAt(std::size_t replayOrdinal,
                      std::uint32_t& commandIndex) const noexcept;
};

EncodePartitionReplayStream makeEncodePartitionReplayStream(
    std::size_t slotIndex,
    const core::ChunkSlot& slot,
    std::size_t commandBegin,
    std::size_t commandCount,
    bool commandOrderActive,
    std::span<const std::uint32_t> commandOrder,
    // Active nonempty orders require at least commandCount entries. The
    // factory initializes this caller-owned scratch and writes each selected
    // command's replay ordinal, proving range and uniqueness without another
    // warm-path allocation. Source order and active empty DCE need no scratch.
    std::span<std::size_t> replayOrdinalByCommandIndex = {},
    core::CpuReadyTape::SourceRef tapeSource = {}) noexcept;

EncodePartitionReplayStream makeEncodePartitionReplayStream(
    std::size_t slotIndex,
    core::SourcePayloadView payload,
    std::uint64_t seqId,
    std::size_t commandBegin,
    std::size_t commandCount,
    bool commandOrderActive,
    std::span<const std::uint32_t> commandOrder,
    std::span<std::size_t> replayOrdinalByCommandIndex = {},
    core::CpuReadyTape::SourceRef tapeSource = {}) noexcept;

// Synthesizes one locator-only entry against the exact current source. Returns
// false if the selected command/draw is malformed or cannot be represented;
// the identity cursor then retains the complete command as CommandSegment.
bool buildEncodePartitionEntrySnapshot(
    const EncodePartitionReplayStream& stream,
    std::size_t replayOrdinal,
    std::uint32_t drawParamIndex,
    EncodePartitionEntrySnapshot& snapshot) noexcept;

enum class EncodePartitionRangeValidation {
  Valid,
  ReplayStreamInvalid,
  ReplayStreamTooLarge,
  CommandSegmentEmpty,
  CommandSegmentHasDrawEntries,
  DrawRunReplayCountInvalid,
  DrawRunEntriesEmpty,
  ReplayOrdinalOverflow,
  ReplayCoverageGap,
  ReplayCoverageOverlap,
  DrawRunCommandMismatch,
  DrawEntryOverflow,
  DrawCoverageGap,
  DrawCoverageOverlap,
  DrawCoveragePartialTail,
  EntryResolutionFailed,
};

struct EncodePartitionRangeValidationResult {
  EncodePartitionRangeValidation validation =
      EncodePartitionRangeValidation::ReplayStreamInvalid;
  EncodePartitionEntryValidation entryValidation =
      EncodePartitionEntryValidation::RetainedSourceIndexOutOfRange;
  std::size_t rangeIndex = 0;

  explicit operator bool() const noexcept {
    return validation == EncodePartitionRangeValidation::Valid;
  }
};

struct EncodePartitionResolution {
  EncodePartitionRangeValidation validation =
      EncodePartitionRangeValidation::ReplayStreamInvalid;
  EncodePartitionEntryValidation entryValidation =
      EncodePartitionEntryValidation::RetainedSourceIndexOutOfRange;
  ResolvedEncodePartition partition{};

  explicit operator bool() const noexcept {
    return validation == EncodePartitionRangeValidation::Valid;
  }
};

EncodePartitionResolution resolveEncodePartition(
    const EncodePartitionRangeSnapshot& range,
    const EncodePartitionReplayStream& stream) noexcept;

// Preflights the complete plan without side effects. Success proves exact
// effective-stream coverage; failure means callers must discard the complete
// plan and start an identity cursor over this same stream.
EncodePartitionRangeValidationResult validateEncodePartitionRanges(
    std::span<const EncodePartitionRangeSnapshot> ranges,
    const EncodePartitionReplayStream& stream) noexcept;

inline constexpr std::uint32_t kProductionPartitionDrawThreshold = 64u;
inline constexpr std::uint32_t kProductionPartitionTargetDraws = 32u;
inline constexpr std::uint32_t kProductionPartitionMinimumDraws = 16u;
inline constexpr std::size_t kProductionPartitionRangeCapacity = 256u;

enum class ProductionPartitionFallbackReason : std::uint8_t {
  None,
  NoEligibleDrawRun,
  ReplayStreamInvalid,
  ReplayStreamTooLarge,
  RangeCapacity,
  SnapshotInvalid,
  MergePreservation,
  ValidationFailed,
  Count,
};

struct ProductionEncodePartitionPlanStorage {
  std::array<EncodePartitionRangeSnapshot,
             kProductionPartitionRangeCapacity> ranges{};
  std::size_t count = 0;

  void reset() noexcept { count = 0; }
  std::span<const EncodePartitionRangeSnapshot> view() const noexcept {
    return std::span<const EncodePartitionRangeSnapshot>(ranges.data(), count);
  }
};

struct ProductionEncodePartitionPlanResult {
  ProductionPartitionFallbackReason fallback =
      ProductionPartitionFallbackReason::NoEligibleDrawRun;
  EncodePartitionRangeValidation validation =
      EncodePartitionRangeValidation::Valid;
  std::uint32_t rangeCount = 0;
  std::uint32_t drawRangeCount = 0;
  std::uint64_t plannedDrawCount = 0;
  std::uint32_t subdividedDrawRunCount = 0;
  std::uint32_t mergePreservedIdentityCount = 0;
  bool explicitPlan = false;

  friend constexpr bool operator==(
      const ProductionEncodePartitionPlanResult&,
      const ProductionEncodePartitionPlanResult&) = default;
};

static_assert(
    std::is_trivially_copyable_v<ProductionEncodePartitionPlanStorage>);
static_assert(std::is_standard_layout_v<ProductionEncodePartitionPlanStorage>);
static_assert(
    std::is_trivially_copyable_v<ProductionEncodePartitionPlanResult>);
static_assert(std::is_standard_layout_v<ProductionEncodePartitionPlanResult>);

// Deterministic production policy over the already-selected effective replay
// stream. Storage is caller-owned, fixed-capacity, and contains locator values
// only. A non-explicit result leaves storage empty and selects identity.
ProductionEncodePartitionPlanResult planProductionEncodePartitions(
    const EncodePartitionReplayStream& stream,
    ProductionEncodePartitionPlanStorage& storage) noexcept;

// Allocation-free identity traversal of the selected effective stream. A
// valid non-empty DrawRun becomes one full DrawRunEntries range; all other
// commands are coalesced into complete-command segments.
class EncodePartitionIdentityCursor {
public:
  explicit EncodePartitionIdentityCursor(
      const EncodePartitionReplayStream& stream) noexcept;

  bool next(EncodePartitionRangeSnapshot& range) noexcept;
  bool next(EncodePartitionRangeSnapshot& range,
            ResolvedEncodePartition& partition) noexcept;

private:
  const EncodePartitionReplayStream* stream_ = nullptr;
  std::size_t replayOrdinal_ = 0;
  bool pendingDrawRun_ = false;
  EncodePartitionRangeSnapshot pendingRange_{};
  ResolvedEncodePartition pendingPartition_{};
};

struct EncodePartitionSerialBatch {
  EncodePartitionRangeKind kind = EncodePartitionRangeKind::CommandSegment;
  std::uint32_t replayOrdinalBegin = 0;
  std::uint32_t replayOrdinalCount = 0;
  // One range for identity traversal; one or more adjacent explicit draw
  // subranges for a DrawRun. The span is valid only until the next next().
  std::span<const EncodePartitionRangeSnapshot> ranges{};
  // Identity DrawRuns carry the already-resolved, call-local full range so
  // serial execution does not repeat source lookup or locator validation.
  // The borrowed view is valid only until the next next().
  bool identityResolved = false;
  ResolvedEncodePartition identityPartition{};
};

// Serial outer cursor shared by Metal execution and pure tests. Explicit draw
// subranges for one replay ordinal are grouped so command-level setup occurs
// exactly once. When useExplicitPlan is false it delegates to identity.
class EncodePartitionSerialCursor {
public:
  EncodePartitionSerialCursor(
      const EncodePartitionReplayStream& stream,
      std::span<const EncodePartitionRangeSnapshot> explicitRanges,
      bool useExplicitPlan) noexcept;

  bool next(EncodePartitionSerialBatch& batch) noexcept;

private:
  std::span<const EncodePartitionRangeSnapshot> explicitRanges_{};
  std::size_t explicitIndex_ = 0;
  bool useExplicitPlan_ = false;
  EncodePartitionIdentityCursor identityCursor_;
  EncodePartitionRangeSnapshot identityRange_{};
  ResolvedEncodePartition identityPartition_{};
};

}  // namespace dxmt9::encoders
