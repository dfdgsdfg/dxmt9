#pragma once

#include "../dxmt9_source_payload.hpp"

namespace dxmt9::render {

bool sessionSourceHasFinalPresentTail(
    core::SourcePayloadView payload) noexcept;
bool sessionSourceCanBeHead(core::SourcePayloadView payload) noexcept;
bool sessionSourceCanAppendToPending(core::SourcePayloadView payload,
                                     bool hasPendingSession) noexcept;
bool sessionShouldSubmitBeforeInitializerWait(
    bool sourceCanAppendToPending,
    bool pendingSessionHasActiveRender,
    bool initializerHasPendingUploads) noexcept;

}  // namespace dxmt9::render
