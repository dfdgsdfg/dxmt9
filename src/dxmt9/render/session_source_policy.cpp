#include "session_source_policy.hpp"

#include <cstddef>

namespace dxmt9::render {

bool sessionSourceHasFinalPresentTail(
    core::SourcePayloadView payload) noexcept {
  const std::size_t count = payload.commandCount();
  return count != 0 &&
         payload.commandAt(count - 1u).kind() ==
             core::MetalCommandKind::Present &&
         payload.presentRecordCount() == 1u;
}

bool sessionSourceCanBeHead(core::SourcePayloadView payload) noexcept {
  return !payload.commandsEmpty() && payload.presentRecordCount() == 0u;
}

bool sessionSourceCanAppendToPending(core::SourcePayloadView payload,
                                     bool hasPendingSession) noexcept {
  if (sessionSourceHasFinalPresentTail(payload)) {
    return true;
  }
  if (!hasPendingSession) {
    return false;
  }
  return sessionSourceCanBeHead(payload);
}

bool sessionShouldSubmitBeforeInitializerWait(
    bool sourceCanAppendToPending,
    bool pendingSessionHasActiveRender,
    bool initializerHasPendingUploads) noexcept {
  return sourceCanAppendToPending &&
         pendingSessionHasActiveRender &&
         initializerHasPendingUploads;
}

}  // namespace dxmt9::render
