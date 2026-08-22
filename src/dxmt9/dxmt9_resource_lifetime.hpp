#pragma once

namespace dxmt9::resources::lifetime {

// TLA+: ResourceLifetime!StageInitializerUpload / NoUseAfterFree. A pending
// initializer upload is an independent owner because it has no chunk seqId.
constexpr bool pendingInitializerReferenceSafe(
    bool pendingUpload, bool retainedDestination) noexcept {
  return !pendingUpload || retainedDestination;
}

}  // namespace dxmt9::resources::lifetime
