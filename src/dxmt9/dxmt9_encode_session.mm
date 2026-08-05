#include "dxmt9_encode_session_internal.hpp"

#include "dxmt9/assert.hpp"

#include <memory>
#include <utility>

namespace dxmt9::encoders {

EncodeChunkSession makeEncodeChunkSession() {
  EncodeChunkSession session(new EncodeChunkSessionState{},
                             EncodeChunkSessionDeleter{});
  session->storage = encode_session::createStorage();
  return session;
}

void EncodeChunkSessionDeleter::operator()(
    EncodeChunkSessionState* session) const noexcept {
  if (!session) {
    return;
  }
  encode_session::destroyStorage(session->storage);
  delete session;
}

void resetEncodeChunkSession(EncodeChunkSessionState& session) {
  DXMT_ASSERT(session.storage);
  encode_session::resetStorage(*session.storage);
  session.sources.clear();
}

bool retainEncodeChunkSessionUntilSubmissionComplete(
    EncodeChunkSession session,
    core::metalqueue::QueueSubmissionRecord& record) {
  if (!session) {
    return true;
  }
  std::shared_ptr<EncodeChunkSessionState> retained(
      session.release(), EncodeChunkSessionDeleter{});
  record.retainedPayloads.push_back(std::move(retained));
  return true;
}

bool encodeChunkSessionHasActiveRender(
    const EncodeChunkSessionState& session) noexcept {
  return session.storage &&
         encode_session::storageHasActiveRender(*session.storage);
}

bool encodeChunkSessionHasDeferredSubmissionPayload(
    const EncodeChunkSessionState& session) noexcept {
  return session.storage &&
         encode_session::storageHasDeferredSubmissionPayload(*session.storage);
}

bool canAppendEncodeChunkSessionSource(
    const EncodeChunkSessionState& session,
    core::metalqueue::QueueCompletionSource source) noexcept {
  return session.sources.canAppend(source);
}

bool appendEncodeChunkSessionSource(
    EncodeChunkSessionState& session,
    core::metalqueue::QueueCompletionSource source) noexcept {
  return session.sources.append(source);
}

std::span<const core::metalqueue::QueueCompletionSource>
encodeChunkSessionSources(const EncodeChunkSessionState& session) noexcept {
  return session.sources.span();
}

std::optional<core::metalqueue::PublishedCommandRef>
encodeChunkSessionPendingClearCommand(
    const EncodeChunkSessionState& session) noexcept {
  return session.storage
             ? encode_session::storagePendingClearCommand(*session.storage)
             : std::nullopt;
}

}  // namespace dxmt9::encoders
