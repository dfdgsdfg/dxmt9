#pragma once

#include "dxmt9_queue.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <type_traits>

namespace dxmt9::encoders {

struct EncodeContext;
struct EncodeChunkSessionState;

// Stable identity for one source retained by an encode session. The index
// selects the source within the retained table; Tape source/storage generations
// plus seqId reject a stale index after temporary-control reuse.
struct RetainedEncodeSourceLocator {
  core::CpuReadyTape::SourceRef tapeSource{};
  std::uint32_t retainedSourceIndex = 0;
  std::uint32_t slotIndex = 0;
  std::uint64_t seqId = 0;

  friend constexpr bool operator==(const RetainedEncodeSourceLocator&,
                                   const RetainedEncodeSourceLocator&) = default;
};

// Immutable-after-publication locator for a single draw entry. Large state,
// uniforms, shader layouts, binding records, and UP data remain owned by the
// retained ChunkSlot. The payload ranges are absolute offsets into that
// source's ChunkSlot::drawPayloadArena. DrawParam stores ranges relative to its
// draw-run payload, so a planner must add DrawRunCommandRecord::payloadOffset
// when publishing a non-empty range into this snapshot.
struct EncodePartitionEntrySnapshot {
  RetainedEncodeSourceLocator source{};
  std::uint32_t commandIndex = 0;
  std::uint32_t drawRunRecordIndex = 0;
  std::uint32_t stateIndex = 0;
  std::uint32_t drawParamIndex = 0;
  core::DrawUniformHandle uniformHandle{};
  core::DrawPayloadRange bindingOverrideBytes{};
  core::DrawPayloadRange bindingSnapshotBytes{};

  friend constexpr bool operator==(const EncodePartitionEntrySnapshot& lhs,
                                   const EncodePartitionEntrySnapshot& rhs) {
    return lhs.source == rhs.source &&
           lhs.commandIndex == rhs.commandIndex &&
           lhs.drawRunRecordIndex == rhs.drawRunRecordIndex &&
           lhs.stateIndex == rhs.stateIndex &&
           lhs.drawParamIndex == rhs.drawParamIndex &&
           lhs.uniformHandle == rhs.uniformHandle &&
           lhs.bindingOverrideBytes.offset == rhs.bindingOverrideBytes.offset &&
           lhs.bindingOverrideBytes.size == rhs.bindingOverrideBytes.size &&
           lhs.bindingSnapshotBytes.offset == rhs.bindingSnapshotBytes.offset &&
           lhs.bindingSnapshotBytes.size == rhs.bindingSnapshotBytes.size;
  }
};

enum class EncodePartitionRangeKind : std::uint32_t {
  CommandSegment,
  DrawRunEntries,
};

// Immutable serial-execution coverage for one effective replay-stream range.
// CommandSegment covers one or more complete replay ordinals and carries no
// draw entries. DrawRunEntries covers a contiguous DrawParam subrange within
// exactly one DrawRun replay ordinal; entry locates its first DrawParam.
struct EncodePartitionRangeSnapshot {
  EncodePartitionRangeKind kind = EncodePartitionRangeKind::CommandSegment;
  std::uint32_t replayOrdinalBegin = 0;
  std::uint32_t replayOrdinalCount = 0;
  std::uint32_t drawEntryCount = 0;
  EncodePartitionEntrySnapshot entry{};

  friend constexpr bool operator==(const EncodePartitionRangeSnapshot&,
                                   const EncodePartitionRangeSnapshot&) =
      default;
};

static_assert(std::is_trivially_copyable_v<RetainedEncodeSourceLocator>);
static_assert(std::is_standard_layout_v<RetainedEncodeSourceLocator>);
static_assert(sizeof(RetainedEncodeSourceLocator) == 48);
static_assert(std::is_trivially_copyable_v<EncodePartitionEntrySnapshot>);
static_assert(std::is_standard_layout_v<EncodePartitionEntrySnapshot>);
static_assert(sizeof(EncodePartitionEntrySnapshot) == 96);
static_assert(std::is_trivially_copyable_v<EncodePartitionRangeSnapshot>);
static_assert(std::is_standard_layout_v<EncodePartitionRangeSnapshot>);
static_assert(sizeof(EncodePartitionRangeSnapshot) == 112);

struct EncodeChunkSessionDeleter {
  void operator()(EncodeChunkSessionState* session) const noexcept;
};

using EncodeChunkSession =
    std::unique_ptr<EncodeChunkSessionState, EncodeChunkSessionDeleter>;

EncodeChunkSession makeEncodeChunkSession();
void resetEncodeChunkSession(EncodeChunkSessionState& session);
bool retainEncodeChunkSessionUntilSubmissionComplete(
    EncodeChunkSession session,
    core::metalqueue::QueueSubmissionRecord& record);
bool encodeChunkSessionHasActiveRender(
    const EncodeChunkSessionState& session) noexcept;
bool encodeChunkSessionHasDeferredSubmissionPayload(
    const EncodeChunkSessionState& session) noexcept;
bool canAppendEncodeChunkSessionSource(
    const EncodeChunkSessionState& session,
    core::metalqueue::QueueCompletionSource source) noexcept;
bool appendEncodeChunkSessionSource(
    EncodeChunkSessionState& session,
    core::metalqueue::QueueCompletionSource source) noexcept;
std::span<const core::metalqueue::QueueCompletionSource>
encodeChunkSessionSources(const EncodeChunkSessionState& session) noexcept;
std::optional<core::metalqueue::PublishedCommandRef>
encodeChunkSessionPendingClearCommand(
    const EncodeChunkSessionState& session) noexcept;

// A deterministic preflight rejection (missing/conflicting command-buffer
// ownership or completion-source mismatch) is side-effect free: record and
// session ownership remain unchanged and the caller may correct the record and
// retry. Once preflight succeeds, finalization ends encoders and publishes the
// session, so callers must treat a later exceptional allocation failure as
// fatal rather than retryable.
bool finalizeEncodeChunkSessionIntoSubmission(
    EncodeContext& ctx,
    EncodeChunkSessionState& session,
    core::metalqueue::QueueSubmissionRecord& record);

}  // namespace dxmt9::encoders
