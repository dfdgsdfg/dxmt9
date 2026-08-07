#pragma once

#include "dxmt9_perf_counters.hpp"

namespace dxmt9::encoders {

// Policy seam for provisional WMTStoreActionUnknown selection. Only bounded
// proof exhaustion is recoverable online; malformed evidence, resolve actions,
// present sources, and sessions with no possible successor stay concrete.
inline constexpr bool lateRenderPassStoreEligible(
    bool blockNoLookahead,
    perf::RenderPassNoLookaheadCause cause,
    bool lookaheadMayHaveFutureSources,
    bool hasResolveTarget,
    bool presentSourceConstrained) noexcept {
  const bool recoverableCause =
      cause == perf::RenderPassNoLookaheadCause::SuffixExhausted ||
      cause == perf::RenderPassNoLookaheadCause::StorageTruncated;
  return blockNoLookahead && recoverableCause &&
      lookaheadMayHaveFutureSources && !hasResolveTarget &&
      !presentSourceConstrained;
}

inline constexpr perf::RenderPassDepthStoreProof legacyDepthProofForPass(
    bool hasDepthAspect,
    perf::RenderPassDepthStoreProof depthProof,
    perf::RenderPassDepthStoreProof stencilProof) noexcept {
  return hasDepthAspect ? depthProof : stencilProof;
}

}  // namespace dxmt9::encoders
