#pragma once

#include "dxmt9_encode_session.hpp"

#include <cstddef>
#include <cstdint>
#include <span>

namespace dxmt9::encoders {

// A resolved entry borrows queue-owned source storage only for the synchronous
// encode call. It must never be retained by EncodeSession or a Metal callback.
struct ResolvedEncodePartitionEntry {
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
  SourceOrdinalOutOfRange,
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
      EncodePartitionEntryValidation::SourceOrdinalOutOfRange;
  ResolvedEncodePartitionEntry entry{};

  explicit operator bool() const noexcept {
    return validation == EncodePartitionEntryValidation::Valid;
  }
};

// Resolves locator-only partition metadata against the call-local retained
// source table. Every identity, command, SoA index, uniform handle, and payload
// range must agree with immutable ChunkSlot storage. Rejection is side-effect
// free so the caller can fail open to source-order serial encoding.
EncodePartitionEntryResolution resolveEncodePartitionEntry(
    const EncodePartitionEntrySnapshot& snapshot,
    std::span<const core::metalqueue::ReadySlotSnapshot> sources) noexcept;

// Exact current-source view of the already selected effective replay stream.
// `source` is by value, but its ChunkSlot pointer and command-order span are
// borrowed only for the synchronous encode call.
struct EncodePartitionReplayStream {
  core::metalqueue::ReadySlotSnapshot source{};
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
    std::span<std::size_t> replayOrdinalByCommandIndex = {}) noexcept;

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
      EncodePartitionEntryValidation::SourceOrdinalOutOfRange;
  std::size_t rangeIndex = 0;

  explicit operator bool() const noexcept {
    return validation == EncodePartitionRangeValidation::Valid;
  }
};

struct EncodePartitionResolution {
  EncodePartitionRangeValidation validation =
      EncodePartitionRangeValidation::ReplayStreamInvalid;
  EncodePartitionEntryValidation entryValidation =
      EncodePartitionEntryValidation::SourceOrdinalOutOfRange;
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
  bool drawOnlyCommandStream_ = false;
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
