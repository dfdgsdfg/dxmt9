#pragma once

#include "dxmt9_encode_attribution.hpp"
#include "dxmt9_queue.hpp"
#include "dxmt9_session_finalize_cause.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <type_traits>

namespace dxmt9::encoders {

struct EncodeContext;
struct EncodeChunkSessionState;

inline constexpr std::size_t kActiveRenderDependencyCapacity =
    core::kMaxRenderTargets + 1u;

// Immutable value snapshot of the render pass currently owned by an
// EncodeSession. The dependency list is the complete deduplicated set of
// attachment writes made by that active encoder. It is bounded so FrameGraph
// planning can consume it synchronously without retaining session storage or
// Metal objects. `dependencyCount` greater than the fixed capacity and
// `complete == false` are explicit conservative-fallback states.
struct ActiveRenderDependencySnapshot {
  std::array<core::Handle, core::kMaxRenderTargets> colorAttachments{};
  core::Handle depthStencil{};
  std::array<core::Handle, kActiveRenderDependencyCapacity>
      writeDependencies{};
  std::uint32_t dependencyCount = 0;
  std::uint32_t sampleCount = 1;
  bool complete = false;

  friend constexpr bool operator==(const ActiveRenderDependencySnapshot&,
                                   const ActiveRenderDependencySnapshot&) =
      default;
};

static_assert(std::is_trivially_copyable_v<ActiveRenderDependencySnapshot>);
static_assert(std::is_standard_layout_v<ActiveRenderDependencySnapshot>);

// Typed proof state at the replay frontier. CleanClosedEncoderNoPendingClear
// means that no Metal encoder or deferred clear is live; it does not assert
// that the retained command buffer contains no earlier work. InjectedUnknown
// is selected by FrameGraphBackend for an injected command buffer that has no
// EncodeSession storage to prove its encoder lifecycle.
enum class EncodeSessionReplayFrontierState : std::uint8_t {
  CleanClosedEncoderNoPendingClear,
  PendingClear,
  ActiveRenderComplete,
  ActiveRenderUnproved,
  ActiveBlitUnsupported,
  InjectedUnknown,
};

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
std::optional<ActiveRenderDependencySnapshot>
encodeChunkSessionActiveRenderDependencySnapshot(
    const EncodeChunkSessionState& session) noexcept;
std::optional<RenderPassInstanceToken>
encodeChunkSessionActiveRenderInstanceToken(
    const EncodeChunkSessionState& session) noexcept;
EncodeSessionReplayFrontierState encodeChunkSessionReplayFrontierState(
    const EncodeChunkSessionState& session) noexcept;
bool encodeChunkSessionHasDeferredSubmissionPayload(
    const EncodeChunkSessionState& session) noexcept;
bool canAppendEncodeChunkSessionSource(
    const EncodeChunkSessionState& session,
    core::metalqueue::QueueCompletionSource source) noexcept;
bool appendEncodeChunkSessionSource(
    EncodeChunkSessionState& session,
    core::metalqueue::QueueCompletionSource source) noexcept;
bool appendEncodeChunkSessionSources(
    EncodeChunkSessionState& session,
    std::span<const core::metalqueue::QueueCompletionSource> sources) noexcept;
bool setEncodeChunkSessionSourceOwner(
    EncodeChunkSessionState& session, std::uint64_t seqId,
    ::dxmt9::queue::PipelineOwner owner) noexcept;
bool replaceEncodeChunkSessionSourceIdentity(
    EncodeChunkSessionState& session,
    const core::metalqueue::QueueCompletionSource& expected,
    const core::metalqueue::QueueCompletionSource& replacement) noexcept;
std::span<const core::metalqueue::QueueCompletionSource>
encodeChunkSessionSources(const EncodeChunkSessionState& session) noexcept;
std::optional<core::metalqueue::PublishedCommandRef>
encodeChunkSessionPendingClearCommand(
    const EncodeChunkSessionState& session) noexcept;

enum class EncodeChunkSessionPassCloseResult : std::uint8_t {
  NoActivePass,
  Closed,
  InvalidCommandBufferCarrier,
};

// End only the active render pass while retaining the EncodeSession and its
// command-buffer chain. This is the production ordered-control ClosePass
// primitive: it neither finalizes nor submits the carrier, and all shadows,
// sidecars, callbacks, and retained completion sources remain session-owned so
// later sources can resume encoding into the same command buffer. Calling it
// when no render pass is active is an idempotent no-op. An active pass requires
// the current session command-buffer carrier; rejection is side-effect free.
EncodeChunkSessionPassCloseResult closeEncodeChunkSessionRenderPass(
    EncodeContext& ctx,
    EncodeChunkSessionState& session,
    core::metalqueue::QueueSubmissionRecord& commandBufferCarrier);

// A deterministic preflight rejection (missing/conflicting command-buffer
// ownership or completion-source mismatch) is side-effect free: record and
// session ownership remain unchanged and the caller may correct the record and
// retry. Once preflight succeeds, finalization ends encoders and publishes the
// session, so callers must treat a later exceptional allocation failure as
// fatal rather than retryable.
bool finalizeEncodeChunkSessionIntoSubmission(
    EncodeContext& ctx,
    EncodeChunkSessionState& session,
    core::metalqueue::QueueSubmissionRecord& record,
    SessionFinalizeCause cause = SessionFinalizeCause::FailOrOther);

}  // namespace dxmt9::encoders
