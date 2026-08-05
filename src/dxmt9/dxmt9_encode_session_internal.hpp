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

EncodeChunkSessionStorage* createStorage();
void destroyStorage(EncodeChunkSessionStorage* storage) noexcept;
void resetStorage(EncodeChunkSessionStorage& storage);
bool storageHasActiveRender(
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
