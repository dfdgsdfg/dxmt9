#pragma once

#include "dxmt9_queue.hpp"

#include <cstdint>
#include <memory>
#include <span>
#include <type_traits>

namespace dxmt9::encoders {

struct EncodeContext;
struct EncodeChunkSessionState;

// Stable identity for one source retained by an encode session. The ordinal
// selects the source within the retained table; slotIndex + seqId reject a
// stale ordinal after queue-slot reuse.
struct RetainedEncodeSourceLocator {
  std::uint32_t sourceOrdinal = 0;
  std::uint32_t slotIndex = 0;
  std::uint64_t seqId = 0;

  friend constexpr bool operator==(const RetainedEncodeSourceLocator&,
                                   const RetainedEncodeSourceLocator&) = default;
};

// Immutable-after-publication locator for a single draw entry. Large state,
// uniforms, shader layouts, binding records, and UP data remain owned by the
// retained ChunkSlot. The payload ranges are absolute offsets into that
// source's ChunkSlot::drawPayloadArena. This is metadata only: the current
// serial executor intentionally ignores it until a retained-source resolver
// exists.
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

static_assert(std::is_trivially_copyable_v<RetainedEncodeSourceLocator>);
static_assert(std::is_standard_layout_v<RetainedEncodeSourceLocator>);
static_assert(sizeof(RetainedEncodeSourceLocator) == 16);
static_assert(std::is_trivially_copyable_v<EncodePartitionEntrySnapshot>);
static_assert(std::is_standard_layout_v<EncodePartitionEntrySnapshot>);
static_assert(sizeof(EncodePartitionEntrySnapshot) == 64);

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

bool finalizeEncodeChunkSessionIntoSubmission(
    EncodeContext& ctx,
    EncodeChunkSessionState& session,
    core::metalqueue::QueueSubmissionRecord& record);

}  // namespace dxmt9::encoders
