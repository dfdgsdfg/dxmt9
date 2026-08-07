#pragma once

#include "dxmt9_encode_session.hpp"

namespace dxmt9::encoders {

namespace encode_session {

struct EncodeChunkSessionStorage;

struct EncodeCallState {
  WMT::Reference<WMT::CommandBuffer> commandBuffer{};
  std::uint64_t committedSubCommandBuffers = 0;
  bool commandBufferHasWork = false;
  bool captureAlreadyStartedAtChunkBegin = false;
};

// Pure projection of the mutable storage fields that determine whether a
// source-local replay plan may cross the current session frontier. Keeping the
// pending-clear payload and command sidecar as separate facts makes a partial
// or malformed carry state conservative instead of accidentally proving a
// clean frontier.
struct ReplayFrontierFacts {
  bool hasPendingClearPayload = false;
  bool hasPendingClearCommand = false;
  bool hasActiveRenderEncoder = false;
  bool hasActiveBlitEncoder = false;
  bool hasActiveRender = false;
  bool activeRenderSnapshotComplete = false;
};

constexpr EncodeSessionReplayFrontierState replayFrontierStateForFacts(
    ReplayFrontierFacts facts) noexcept {
  using State = EncodeSessionReplayFrontierState;
  if (facts.hasPendingClearPayload || facts.hasPendingClearCommand) {
    return State::PendingClear;
  }
  if (facts.hasActiveBlitEncoder) {
    return State::ActiveBlitUnsupported;
  }
  if (facts.hasActiveRenderEncoder || facts.hasActiveRender) {
    return facts.activeRenderSnapshotComplete
               ? State::ActiveRenderComplete
               : State::ActiveRenderUnproved;
  }
  return State::CleanClosedEncoderNoPendingClear;
}

EncodeChunkSessionStorage* createStorage();
void destroyStorage(EncodeChunkSessionStorage* storage) noexcept;
void resetStorage(EncodeChunkSessionStorage& storage);
bool storageHasActiveRender(
    const EncodeChunkSessionStorage& storage) noexcept;
std::optional<ActiveRenderDependencySnapshot>
storageActiveRenderDependencySnapshot(
    const EncodeChunkSessionStorage& storage) noexcept;
std::optional<RenderPassInstanceToken> storageActiveRenderInstanceToken(
    const EncodeChunkSessionStorage& storage) noexcept;
EncodeSessionReplayFrontierState storageReplayFrontierState(
    const EncodeChunkSessionStorage& storage) noexcept;
bool storageHasDeferredSubmissionPayload(
    const EncodeChunkSessionStorage& storage) noexcept;
std::optional<core::metalqueue::PublishedCommandRef>
storagePendingClearCommand(
    const EncodeChunkSessionStorage& storage) noexcept;

}  // namespace encode_session

struct EncodeChunkSessionState {
  encode_session::EncodeChunkSessionStorage* storage = nullptr;
  core::metalqueue::EncodeSessionSourceList sources{};
};

}  // namespace dxmt9::encoders
